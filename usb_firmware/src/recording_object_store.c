/* SPDX-License-Identifier: Apache-2.0 */
#include "recording_object_store.h"
#include <limits.h>
#include <stdbool.h>
#include <string.h>

enum { ROOT_ACTIVE=1,ROOT_FINAL=2,SLOT_INTENT=3,SLOT_COMMIT=4,SLOT_RETIRED=5 };
_Static_assert(ROS_CONTAINER_BYTES<=ROS_DATA_PAGES*2048U,"Fixed slot capacity must cover full pipeline container");
static const uint8_t magic[8]={'O','P','N','D','O','S','1',0};
static const uint8_t extent_magic[8]={'O','P','N','D','O','S','2',0};
static const uint8_t full_magic[8]={'O','P','N','D','O','S','3',0};
static const uint8_t *format_tag(const struct ros_context *c)
{return c->extent_profile==2?full_magic:c->extent_profile?extent_magic:magic;}
static void wipe(void *v,size_t n){volatile uint8_t *p=v;while(n--)*p++=0;}
static bool fill(const uint8_t *p,size_t n,uint8_t v){while(n--)if(*p++!=v)return false;return true;}
static bool overlap(const void *a,size_t an,const void *b,size_t bn)
{uintptr_t x=(uintptr_t)a,y=(uintptr_t)b;return an>UINTPTR_MAX-x||bn>UINTPTR_MAX-y||(x<y+bn&&y<x+an);}
static bool external(const struct ros_context *c,const void *p,size_t n)
{return p&&n&&!overlap(c,sizeof(*c),p,n);}
static void put(uint8_t *p,uint64_t v,unsigned n){for(unsigned i=0;i<n;++i)p[i]=(uint8_t)(v>>(8U*i));}
static uint64_t get(const uint8_t *p,unsigned n){uint64_t v=0;for(unsigned i=0;i<n;++i)v|=(uint64_t)p[i]<<(8U*i);return v;}
static bool id(const uint8_t *p,size_t n){return !fill(p,n,0)&&!fill(p,n,255);}
static bool volume_equal(const struct es_binding *a,const struct es_binding *b)
{return a->generation==b->generation&&!memcmp(a->key_fingerprint,b->key_fingerprint,32)&&
 !memcmp(a->device_id,b->device_id,16)&&!memcmp(a->volume_id,b->volume_id,16);}
static bool identity_equal(const struct es_binding *a,const struct es_binding *b)
{return volume_equal(a,b)&&!memcmp(a->recording_id,b->recording_id,16);}
static bool retired(const struct ros_context *c,const uint8_t uuid[16])
{for(uint32_t i=0;i<c->retired_count;++i)if(!memcmp(c->retired[i],uuid,16))return true;return false;}
static uint32_t sector(const struct ros_context *c,uint32_t slot){return c->root_count+slot*ROS_SLOT_PAGES;}
static uint32_t slot_state(const struct ros_context *c,uint32_t slot)
{return (c->used[slot/2U]>>((slot%2U)*4U))&15U;}
static void set_slot_state(struct ros_context *c,uint32_t slot,uint32_t state)
{unsigned shift=(slot%2U)*4U;c->used[slot/2U]=(uint8_t)((c->used[slot/2U]&~(15U<<shift))|(state<<shift));}
static int lock(struct ros_context *c)
{unsigned expected=0;if(!c||c->initialized!=0x524f5331U)return ROS_ARGUMENT;
 return atomic_compare_exchange_strong(&c->gate,&expected,1)?ROS_OK:ROS_BUSY;}
static int unlock(struct ros_context *c,int rc){atomic_store(&c->gate,0);return rc;}
static void buffers(struct ros_context *c)
{wipe(c->container,sizeof(c->container));wipe(c->catalog,sizeof(c->catalog));wipe(c->page,sizeof(c->page));}
static int fault(struct ros_context *c,int rc)
{c->status.fault=1;c->status.ready=0;c->status.error=rc;wipe(&c->receipt,sizeof(c->receipt));
 wipe(&c->final_receipt,sizeof(c->final_receipt));buffers(c);return ROS_FAILED;}
static int guard(struct ros_context *c)
{
 uint64_t before=c->port.now_ms(c->port.user);int cancelled=c->port.cancelled(c->port.user);
 uint64_t now=c->port.now_ms(c->port.user);
 if(before<c->last_now||now<before||now>=c->deadline||cancelled!=0)return fault(c,ROS_FAILED);
 c->last_now=now;return ROS_OK;
}
static int admit(struct ros_context *c,int writing)
{if(guard(c))return ROS_FAILED;int rc=c->port.admit(c->port.user,writing,c->deadline);
 return guard(c)?ROS_FAILED:(rc?fault(c,rc):ROS_OK);}
static int hash(struct ros_context *c,const uint8_t *p,size_t n,uint8_t out[32])
{size_t actual=0;if(guard(c))return ROS_FAILED;int rc=c->hash.sha256(c->hash.user,p,n,out,32,&actual);
 return guard(c)?ROS_FAILED:((rc||actual!=32)?fault(c,rc?rc:ROS_FAILED):ROS_OK);}
static int read_page(struct ros_context *c,uint32_t p)
{if(admit(c,0))return ROS_FAILED;int rc=c->port.read(c->port.user,p,c->page,c->deadline);
 if(guard(c))return ROS_FAILED;
 if(rc!=ROS_READ_OK&&rc!=ROS_READ_MISSING)return fault(c,rc);
 return rc;}
static int write_page(struct ros_context *c,uint32_t p,const uint8_t *data)
{if(admit(c,1))return ROS_FAILED;int rc=c->port.write(c->port.user,p,data,c->deadline);
 return guard(c)?ROS_FAILED:(rc?fault(c,rc):ROS_OK);}
static int sync_map(struct ros_context *c)
{if(admit(c,1))return ROS_FAILED;int rc=c->port.sync(c->port.user,c->deadline);
 return guard(c)?ROS_FAILED:(rc?fault(c,rc):ROS_OK);}
static int begin(struct ros_context *c,uint32_t job,uint64_t deadline)
{
 if(c->status.fault||c->status.job!=ROS_NONE)return ROS_STATE;
 uint64_t now=c->port.now_ms(c->port.user);
 if(now<c->last_now)return fault(c,ROS_FAILED);
 uint64_t max_ms=c->extent_profile&&job==ROS_RECOVER?ROS_EXTENT_RECOVER_MS:ROS_JOB_MAX_MS;
 if(deadline<=now||deadline-now>max_ms)return ROS_ARGUMENT;
 c->last_now=now;c->deadline=deadline;c->status.job=job;c->status.stage=0;c->status.ready=0;
 c->status.error=0;c->index=0;buffers(c);wipe(&c->receipt,sizeof(c->receipt));
 wipe(&c->final_receipt,sizeof(c->final_receipt));return ROS_OK;
}
static void encode_identity(uint8_t *p,const struct es_binding *b)
{memcpy(p+24,b->key_fingerprint,32);memcpy(p+56,b->device_id,16);memcpy(p+72,b->volume_id,16);
 memcpy(p+88,b->recording_id,16);put(p+104,b->generation,8);put(p+112,b->segment_sequence,4);put(p+116,b->plaintext_bytes,4);}
static void decode_identity(const uint8_t *p,struct es_binding *b)
{memset(b,0,sizeof(*b));memcpy(b->key_fingerprint,p+24,32);memcpy(b->device_id,p+56,16);
 memcpy(b->volume_id,p+72,16);memcpy(b->recording_id,p+88,16);b->generation=get(p+104,8);
 b->segment_sequence=(uint32_t)get(p+112,4);b->plaintext_bytes=(uint32_t)get(p+116,4);}
static int catalog_start(struct ros_context *c,uint32_t kind,uint32_t where,uint32_t root,const struct es_binding *b)
{memset(c->catalog,255,sizeof(c->catalog));memset(c->catalog,0,224);memcpy(c->catalog,format_tag(c),8);
 put(c->catalog+8,c->extent_profile+1U,2);put(c->catalog+10,kind,2);put(c->catalog+12,where,4);put(c->catalog+16,root,4);
 encode_identity(c->catalog,b);return ROS_OK;}
static int catalog_root(struct ros_context *c,uint32_t kind,const struct ros_record *r)
{
 catalog_start(c,kind,c->root,c->root,&r->identity);put(c->catalog+160,r->first_sample,8);
 put(c->catalog+168,kind==ROOT_FINAL?r->completion.next_sample:r->first_sample,8);
 put(c->catalog+184,r->mode,4);put(c->catalog+216,2,4);
 if(kind==ROOT_FINAL){const struct rp_completion *f=&r->completion;put(c->catalog+188,f->reason,4);
  put(c->catalog+192,f->segments,4);put(c->catalog+196,f->gaps,4);put(c->catalog+200,f->captured_samples,8);
  put(c->catalog+208,f->committed_samples,8);}
 return hash(c,c->catalog,224,c->catalog+224);
}
static int catalog_segment(struct ros_context *c,uint32_t kind)
{
 const struct ros_segment *s=&c->segment;catalog_start(c,kind,sector(c,s->slot),s->root,&s->identity);
 put(c->catalog+120,s->container_bytes,4);put(c->catalog+124,s->pages,4);memcpy(c->catalog+128,s->digest,32);
 put(c->catalog+160,s->first_sample,8);put(c->catalog+168,s->next_sample,8);put(c->catalog+176,s->samples,8);
 put(c->catalog+184,c->records[s->root].mode,4);put(c->catalog+216,s->profile,4);put(c->catalog+220,s->gap,4);
 return hash(c,c->catalog,224,c->catalog+224);
}
static int catalog_check(struct ros_context *c,uint32_t where,uint32_t *kind,struct es_binding *b)
{
 uint8_t digest[32];const uint8_t *p=c->page;
 if(memcmp(p,format_tag(c),8)||get(p+8,2)!=c->extent_profile+1U||get(p+12,4)!=where||!fill(p+20,4,0)||
    !fill(p+256,1792,255)||get(p+216,4)!=2)return fault(c,ROS_FAILED);
 if(hash(c,p,224,digest))return ROS_FAILED;
 if(memcmp(digest,p+224,32))return fault(c,ROS_FAILED);
 decode_identity(p,b);*kind=(uint32_t)get(p+10,2);
 if(!volume_equal(b,&c->volume)||!id(b->recording_id,16))return fault(c,ROS_FAILED);
 return ROS_OK;
}
static int parse_root(struct ros_context *c,uint32_t root)
{
 struct es_binding b;uint32_t kind;const uint8_t *p=c->page;
 if(catalog_check(c,root,&kind,&b))return ROS_FAILED;
 if((kind!=ROOT_ACTIVE&&kind!=ROOT_FINAL)||get(p+16,4)!=root||b.segment_sequence||b.plaintext_bytes||
    !fill(p+120,40,0)||get(p+176,8)||get(p+184,4)>1||get(p+220,4)||get(p+168,8)<get(p+160,8))return fault(c,ROS_FAILED);
 for(uint32_t i=0;i<root;++i)if(c->records[i].present&&identity_equal(&b,&c->records[i].identity))return fault(c,ROS_FAILED);
 struct ros_record *r=&c->records[root];memset(r,0,sizeof(*r));r->identity=b;r->present=1;r->profile=2;
 r->finalized=kind==ROOT_FINAL;r->interrupted=!r->finalized;r->mode=(uint32_t)get(p+184,4);
 r->first_sample=get(p+160,8);r->next_sample=r->first_sample;
 if(kind==ROOT_ACTIVE){if(get(p+168,8)!=r->first_sample||!fill(p+188,28,0))return fault(c,ROS_FAILED);}
 else {struct rp_completion *f=&r->completion;f->session=b;f->mode=r->mode;f->reason=(uint32_t)get(p+188,4);
  f->segments=(uint32_t)get(p+192,4);f->gaps=(uint32_t)get(p+196,4);f->captured_samples=get(p+200,8);
  f->committed_samples=get(p+208,8);f->next_sample=get(p+168,8);
  if(!f->reason||f->reason>RP_REASON_LIMIT||f->segments>c->segment_limit||f->captured_samples<f->committed_samples||
     f->next_sample-r->first_sample<f->captured_samples)return fault(c,ROS_FAILED);}
 if(retired(c,b.recording_id))memset(r,0,sizeof(*r));
 return ROS_OK;
}
static int parse_segment(struct ros_context *c,uint32_t slot,uint32_t *kind)
{
 struct es_binding b;const uint8_t *p=c->page;
 if(catalog_check(c,sector(c,slot),kind,&b))return ROS_FAILED;
 uint32_t root=(uint32_t)get(p+16,4),bytes=(uint32_t)get(p+120,4),pages=(uint32_t)get(p+124,4);
 if((*kind!=SLOT_INTENT&&*kind!=SLOT_COMMIT)||root>=c->root_count||
    !b.plaintext_bytes||b.plaintext_bytes>RP_PLAINTEXT_CAPACITY||
    b.segment_sequence>=c->segment_limit||bytes!=b.plaintext_bytes+209U||pages!=(bytes+2047U)/2048U||pages>ROS_DATA_PAGES||
    get(p+184,4)>1||!fill(p+188,28,0)||get(p+220,4)>1||
    get(p+168,8)<=get(p+160,8)||get(p+168,8)-get(p+160,8)!=get(p+176,8)||
    !get(p+176,8)||get(p+176,8)>160000U||get(p+176,8)%320U)return fault(c,ROS_FAILED);
 /* Validate envelope/hash/volume/shape BEFORE consulting durable deletion.
  * A retired slot can outlive reuse of its former root's logical address. */
 if(retired(c,b.recording_id)){*kind=SLOT_RETIRED;return ROS_OK;}
 if(!c->records[root].present||!identity_equal(&b,&c->records[root].identity)||
    get(p+184,4)!=c->records[root].mode)return fault(c,ROS_FAILED);
 struct ros_segment *s=&c->segment;memset(s,0,sizeof(*s));s->identity=b;s->root=root;s->slot=slot;
 s->container_bytes=bytes;s->pages=pages;s->first_sample=get(p+160,8);s->next_sample=get(p+168,8);
 s->samples=get(p+176,8);s->profile=2;s->gap=(uint32_t)get(p+220,4);memcpy(s->digest,p+128,32);return ROS_OK;
}
static int sequence_valid(struct ros_context *c)
{struct ros_record *r=&c->records[c->segment.root];struct ros_segment *s=&c->segment;
 return r->blocked||s->identity.segment_sequence!=r->segments||s->first_sample<r->next_sample||
  s->gap!=(uint32_t)(s->first_sample>r->next_sample)||r->committed_samples>UINT64_MAX-s->samples?fault(c,ROS_FAILED):ROS_OK;}
static void account(struct ros_context *c)
{struct ros_record *r=&c->records[c->segment.root];++r->segments;r->gaps+=c->segment.gap;
 r->committed_samples+=c->segment.samples;r->next_sample=c->segment.next_sample;}
static int complete_valid(struct ros_context *c,const struct ros_record *r,const struct rp_completion *f)
{return !identity_equal(&r->identity,&f->session)||f->session.segment_sequence||f->session.plaintext_bytes||
 f->mode!=r->mode||!f->reason||f->reason>RP_REASON_LIMIT||f->segments!=r->segments||f->gaps<r->gaps||
 f->committed_samples!=r->committed_samples||f->captured_samples<f->committed_samples||
 f->next_sample<r->next_sample||f->next_sample<r->first_sample||f->next_sample-r->first_sample<f->captured_samples?
 fault(c,ROS_FAILED):ROS_OK;}
static int container_valid(struct ros_context *c)
{uint8_t digest[32];if(es_container_validate(c->container,c->segment.container_bytes,&c->segment.identity)||
 hash(c,c->container,c->segment.container_bytes,digest))return fault(c,ROS_FAILED);
 return memcmp(digest,c->segment.digest,32)?fault(c,ROS_FAILED):ROS_OK;}
static int ready(struct ros_context *c){if(guard(c))return ROS_FAILED;c->status.ready=1;return ROS_READY;}
static int read_container_page(struct ros_context *c)
{
 int rc=read_page(c,sector(c,c->segment.slot)+1U+c->index);if(rc!=ROS_READ_OK)return fault(c,rc?rc:ROS_FAILED);
 size_t offset=(size_t)c->index*2048U,n=c->segment.container_bytes-offset;if(n>2048U)n=2048U;
 if(!fill(c->page+n,2048U-n,255))return fault(c,ROS_FAILED);
 memcpy(c->container+offset,c->page,n);++c->index;return ROS_PENDING;
}
static int initialize(struct ros_context *c,const struct ros_port *p,const struct owned_page_hash *h,
 const struct es_binding *v,const uint8_t recipient[65],uint32_t capacity,uint32_t extent)
{
 if(!c||c->initialized||!external(c,p,sizeof(*p))||!external(c,h,sizeof(*h))||!external(c,v,sizeof(*v))||
 !external(c,recipient,65)||!p->now_ms||!p->cancelled||!p->admit||!p->read||!p->write||!p->sync||!p->seal||
 !h->sha256||recipient[0]!=4||!id(v->key_fingerprint,32)||!id(v->device_id,16)||!id(v->volume_id,16)||
 v->generation==0||v->generation>INT64_MAX||!fill(v->recording_id,16,0)||v->segment_sequence||v->plaintext_bytes||
 capacity<ROS_ROOTS+ROS_SLOT_PAGES)return ROS_ARGUMENT;
 uint8_t digest[32];size_t actual=0;
 static const uint8_t domain[]="OpenPendant recipient public key v1";
 _Static_assert(sizeof(domain)==36,"Fingerprint domain");
 /* Domain includes terminal NUL; suite0010 BE then complete uncompressed point. */
 uint8_t fingerprint_input[sizeof(domain)+2U+65U];memcpy(fingerprint_input,domain,sizeof(domain));
 fingerprint_input[sizeof(domain)]=0;fingerprint_input[sizeof(domain)+1U]=16;
 memcpy(fingerprint_input+sizeof(domain)+2U,recipient,65);
 if(h->sha256(h->user,fingerprint_input,sizeof(fingerprint_input),digest,32,&actual)||actual!=32||
 memcmp(digest,v->key_fingerprint,32))return ROS_ARGUMENT;
 memset(c,0,sizeof(*c));atomic_init(&c->gate,0);c->port=*p;c->hash=*h;c->volume=*v;memcpy(c->recipient,recipient,65);
 c->extent_profile=extent;c->root_count=extent?ROS_EXTENT_ROOTS:ROS_ROOTS;
 c->segment_limit=extent==2?RLL_SLOTS:extent?ROS_EXTENT_SLOTS:ROS_SLOTS;
 c->status.slot_count=(capacity-c->root_count)/ROS_SLOT_PAGES;
 if(c->status.slot_count>c->segment_limit)c->status.slot_count=c->segment_limit;
 c->active_root=c->root_count;c->initialized=0x524f5331U;return ROS_OK;
}
int ros_init(struct ros_context *c,const struct ros_port *p,const struct owned_page_hash *h,
 const struct es_binding *v,const uint8_t recipient[65],uint32_t capacity)
{return initialize(c,p,h,v,recipient,capacity,0);}
int ros_init_extent(struct ros_context *c,const struct ros_port *p,const struct owned_page_hash *h,
 const struct es_binding *v,const uint8_t recipient[65])
{return initialize(c,p,h,v,recipient,ROS_EXTENT_CAPACITY,1);}
int ros_init_full(struct ros_context *c,const struct ros_port *p,const struct owned_page_hash *h,
 const struct es_binding *v,const uint8_t recipient[65])
{return initialize(c,p,h,v,recipient,RLL_ROS_CAPACITY,2);}
int ros_recover(struct ros_context *c,uint64_t deadline)
{int rc=lock(c);if(rc)return rc;if(c->status.mounted)return unlock(c,ROS_STATE);
 rc=begin(c,ROS_RECOVER,deadline);if(!rc){c->scan_root=c->scan_slot=0;memset(c->records,0,sizeof(c->records));
 memset(c->used,0,sizeof(c->used));c->status.consumed_slots=0;}return unlock(c,rc?rc:ROS_PENDING);}
int ros_recover_retired(struct ros_context *c,const struct es_binding *v,
 const uint8_t *ids,uint32_t count,uint64_t deadline)
{
 int rc=lock(c);if(rc)return rc;
 if(!external(c,v,sizeof(*v))||!volume_equal(v,&c->volume)||v->segment_sequence||v->plaintext_bytes||
    !fill(v->recording_id,16,0)||count>(c->extent_profile==2?RLL_TOMBSTONES:ROS_RETIRED_MAX)||
    (count&&(!external(c,ids,(size_t)count*16U)||overlap(v,sizeof(*v),ids,(size_t)count*16U))))return unlock(c,ROS_ARGUMENT);
 for(uint32_t i=0;i<count;++i){
  if(!id(ids+i*16U,16))return unlock(c,ROS_ARGUMENT);
  for(uint32_t j=0;j<i;++j)if(!memcmp(ids+i*16U,ids+j*16U,16))return unlock(c,ROS_ARGUMENT);
 }
 for(uint32_t i=0;i<c->retired_count;++i){bool found=false;
  for(uint32_t j=0;j<count;++j)if(!memcmp(c->retired[i],ids+j*16U,16)){found=true;break;}
  if(!found)return unlock(c,ROS_STATE); /* No tombstone rollback/pruning. */
 }
 if(c->active_root<c->root_count)return unlock(c,ROS_STATE);
 rc=begin(c,ROS_RECOVER,deadline);if(rc)return unlock(c,rc);
 if(c->status.mounted&&count==c->retired_count){c->status.job=ROS_NONE;return unlock(c,ROS_OK);}
 memset(c->retired,0,sizeof(c->retired));if(count)memcpy(c->retired,ids,(size_t)count*16U);c->retired_count=count;
 c->status.mounted=0;c->scan_root=c->scan_slot=0;memset(c->records,0,sizeof(c->records));
 memset(c->used,0,sizeof(c->used));c->status.consumed_slots=0;
 return unlock(c,ROS_PENDING);
}
int ros_reserve(struct ros_context *c,const struct es_binding *b,enum rp_mode mode,uint64_t first,uint64_t deadline)
{
 int rc=lock(c);if(rc)return rc;
 if(!external(c,b,sizeof(*b))||!volume_equal(b,&c->volume)||!id(b->recording_id,16)||b->plaintext_bytes||b->segment_sequence||(mode!=RP_MANUAL&&mode!=RP_CONTINUOUS))return unlock(c,ROS_ARGUMENT);
 if(!c->status.mounted||c->active_root<c->root_count||retired(c,b->recording_id))return unlock(c,ROS_STATE);
 uint32_t free_root=c->root_count;for(uint32_t i=0;i<c->root_count;++i){if(c->records[i].present){if(identity_equal(b,&c->records[i].identity))return unlock(c,ROS_STATE);}else if(free_root==c->root_count)free_root=i;}
 if(free_root==c->root_count||c->status.consumed_slots==c->status.slot_count)return unlock(c,ROS_FULL);
 rc=begin(c,ROS_RESERVE,deadline);if(rc)return unlock(c,rc);c->root=free_root;
 memset(&c->pending_record,0,sizeof(c->pending_record));c->pending_record.identity=*b;c->pending_record.present=1;
 c->pending_record.mode=(uint32_t)mode;c->pending_record.profile=2;c->pending_record.first_sample=first;c->pending_record.next_sample=first;
 rc=catalog_root(c,ROOT_ACTIVE,&c->pending_record);return unlock(c,rc?rc:ROS_PENDING);
}
static int seal_prepared(struct ros_context *c,const uint8_t *plain,size_t n)
{
 struct ros_segment *s=&c->segment;const struct es_binding *b=&s->identity;
 uint8_t info[161];size_t actual=0;
 if(es_header_build(c->container,128,b)||es_info_build(info,161,c->container,128,b)||admit(c,1))return fault(c,ROS_FAILED);
 int rc=c->port.seal(c->port.user,c->recipient,info,plain,n,c->container+128,c->container+193,n+16,&actual);
 wipe(info,sizeof(info));
 if(guard(c)||rc||actual!=n+16||es_container_validate(c->container,n+209,b))return fault(c,rc?rc:ROS_FAILED);
 if(hash(c,c->container,n+209,s->digest)||catalog_segment(c,SLOT_INTENT))return ROS_FAILED;
 return ROS_PENDING;
}
static int prepare(struct ros_context *c,const struct es_binding *b,const uint8_t *plain,size_t n,uint64_t deadline,int deferred)
{
 int rc=lock(c);if(rc)return rc;
 if(!external(c,b,sizeof(*b))||!external(c,plain,n)||overlap(b,sizeof(*b),plain,n)||n>RP_PLAINTEXT_CAPACITY||
 n!=b->plaintext_bytes||rp_segment_validate(plain,n)||get(plain+72,4)!=2)return unlock(c,ROS_ARGUMENT);
 if(!c->status.mounted||c->active_root>=c->root_count)return unlock(c,ROS_STATE);
 struct ros_record *r=&c->records[c->active_root];
 if(!identity_equal(b,&r->identity)||b->segment_sequence!=r->segments||b->segment_sequence>=c->segment_limit||r->blocked||r->finalized||r->interrupted)return unlock(c,ROS_STATE);
 uint32_t slot=0;while(slot<c->status.slot_count&&slot_state(c,slot))++slot;if(slot==c->status.slot_count)return unlock(c,ROS_FULL);
 rc=begin(c,ROS_SEGMENT,deadline);if(rc)return unlock(c,rc);
 memset(&c->segment,0,sizeof(c->segment));struct ros_segment *s=&c->segment;s->identity=*b;s->slot=slot;s->root=c->active_root;
 s->container_bytes=(uint32_t)n+209U;s->pages=(s->container_bytes+2047U)/2048U;s->profile=2;
 s->first_sample=get(plain+32,8);s->next_sample=get(plain+40,8);s->samples=get(plain+68,4);s->gap=(uint32_t)(get(plain+28,4)>>1U);
 if(sequence_valid(c))return unlock(c,ROS_FAILED);
 if(deferred){
  /* Copy before returning: the codec immediately wipes/reuses its plaintext.
   * The sole owned container is not published, written or readable until the
   * storage actor seals it in place and completes every existing commit check. */
  memcpy(c->container+193,plain,n);c->status.stage=9;
  return unlock(c,guard(c)?ROS_FAILED:ROS_PENDING);
 }
 return unlock(c,seal_prepared(c,plain,n));
}
int ros_prepare(struct ros_context *c,const struct es_binding *b,const uint8_t *p,size_t n,uint64_t d)
{return prepare(c,b,p,n,d,0);}
int ros_prepare_deferred(struct ros_context *c,const struct es_binding *b,const uint8_t *p,size_t n,uint64_t d)
{return prepare(c,b,p,n,d,1);}
int ros_finalize(struct ros_context *c,const struct rp_completion *f,uint64_t deadline)
{
 int rc=lock(c);if(rc)return rc;if(!external(c,f,sizeof(*f)))return unlock(c,ROS_ARGUMENT);
 if(!c->status.mounted||c->active_root>=c->root_count)return unlock(c,ROS_STATE);
 if(c->status.fault||c->status.job!=ROS_NONE)return unlock(c,ROS_STATE);
 struct ros_record *r=&c->records[c->active_root];if(complete_valid(c,r,f))return unlock(c,ROS_FAILED);
 rc=begin(c,ROS_FINALIZE,deadline);if(rc)return unlock(c,rc);c->root=c->active_root;c->pending_record=*r;
 c->pending_record.finalized=1;c->pending_record.completion=*f;
 rc=catalog_root(c,ROOT_FINAL,&c->pending_record);return unlock(c,rc?rc:ROS_PENDING);
}
int ros_load(struct ros_context *c,uint32_t slot,uint64_t deadline)
{int rc=lock(c);if(rc)return rc;if(!c->status.mounted||slot>=c->status.slot_count||slot_state(c,slot)!=SLOT_COMMIT)return unlock(c,ROS_NOT_FOUND);
 rc=begin(c,ROS_LOAD,deadline);if(!rc)c->slot=slot;return unlock(c,rc?rc:ROS_PENDING);}
int ros_describe_committed(struct ros_context *c,uint32_t slot,uint64_t deadline)
{int rc=lock(c);if(rc)return rc;if(!c->status.mounted||slot>=c->status.slot_count||slot_state(c,slot)!=SLOT_COMMIT)return unlock(c,ROS_NOT_FOUND);
 rc=begin(c,ROS_META,deadline);if(!rc)c->slot=slot;return unlock(c,rc?rc:ROS_PENDING);}
static int recover_step(struct ros_context *c)
{
 int rc;
 if(c->status.stage==0){if(c->scan_root<c->root_count){rc=read_page(c,c->scan_root);
   if(rc==ROS_READ_OK&&parse_root(c,c->scan_root))return ROS_FAILED;
   if(rc<0)return rc;
   ++c->scan_root;return ROS_PENDING;}
  c->status.stage=10;return ROS_PENDING;}
 if(c->status.stage==10){if(c->scan_slot==c->status.slot_count){for(uint32_t i=0;i<c->root_count;++i){struct ros_record *r=&c->records[i];
    if(r->finalized&&(r->blocked||complete_valid(c,r,&r->completion)))return fault(c,ROS_FAILED);}
   c->status.mounted=1;return ready(c);}
  rc=read_page(c,sector(c,c->scan_slot));if(rc<0)return rc;if(rc==ROS_READ_MISSING){++c->scan_slot;return ROS_PENDING;}
  uint32_t kind;if(parse_segment(c,c->scan_slot,&kind))return ROS_FAILED;
  if(kind==SLOT_RETIRED){++c->scan_slot;return ROS_PENDING;}
  if(sequence_valid(c))return ROS_FAILED;
  set_slot_state(c,c->scan_slot,kind);++c->status.consumed_slots;
  if(kind==SLOT_INTENT){c->records[c->segment.root].blocked=1;c->records[c->segment.root].interrupted=1;++c->scan_slot;return ROS_PENDING;}
  /* The new profile rebuilds only a COMMIT index here. Atomic Dhara sync
   * published payload + COMMIT together. Do not read hours of audio to mount;
   * LOAD still reads and verifies the full container before exposing bytes. */
  if(c->extent_profile){account(c);++c->scan_slot;return ROS_PENDING;}
  c->index=0;c->status.stage=11;return ROS_PENDING;}
 if(c->index<c->segment.pages)return read_container_page(c);
 if(container_valid(c))return ROS_FAILED;
 account(c);wipe(c->container,sizeof(c->container));++c->scan_slot;c->status.stage=10;return ROS_PENDING;
}
static int root_step(struct ros_context *c)
{
 if(c->status.stage==0){if(write_page(c,c->root,c->catalog))return ROS_FAILED;++c->status.stage;return ROS_PENDING;}
 if(c->status.stage==1){if(sync_map(c))return ROS_FAILED;++c->status.stage;return ROS_PENDING;}
 if(read_page(c,c->root)!=ROS_READ_OK||memcmp(c->page,c->catalog,2048))return fault(c,ROS_FAILED);
 c->records[c->root]=c->pending_record;
 if(c->status.job==ROS_RESERVE)c->active_root=c->root;
 else {c->active_root=c->root_count;c->final_receipt.completion=c->pending_record.completion;c->final_receipt.durable=1;}
 return ready(c);
}
static int segment_step(struct ros_context *c)
{
 struct ros_segment *s=&c->segment;uint32_t stage=c->status.stage;
 if(stage==9){
  int rc=seal_prepared(c,c->container+193,s->identity.plaintext_bytes);
  if(rc==ROS_PENDING)c->status.stage=0;
  return rc;
 }
 if(stage==0){if(write_page(c,sector(c,s->slot),c->catalog))return ROS_FAILED;++c->status.stage;return ROS_PENDING;}
 if(stage==1){if(sync_map(c))return ROS_FAILED;++c->status.stage;return ROS_PENDING;}
 if(stage==2){if(read_page(c,sector(c,s->slot))!=ROS_READ_OK||memcmp(c->page,c->catalog,2048))return fault(c,ROS_FAILED);
  set_slot_state(c,s->slot,SLOT_INTENT);++c->status.consumed_slots;if(catalog_segment(c,SLOT_COMMIT))return ROS_FAILED;
  c->index=0;++c->status.stage;return ROS_PENDING;}
 /* Payload and COMMIT belong to one map checkpoint. The durable INTENT
  * already excludes this slot after interruption. Dhara's checkpoint includes
  * all preceding logical writes; never report success until sync AND full
  * post-sync payload/catalog readback. Avoid a padded payload-only checkpoint. */
 if(stage==3){if(c->index==s->pages){c->status.stage=6;return ROS_PENDING;}
  size_t offset=(size_t)c->index*2048U,n=s->container_bytes-offset;if(n>2048U)n=2048U;
  memset(c->page,255,2048);memcpy(c->page,c->container+offset,n);
  if(write_page(c,sector(c,s->slot)+1U+c->index,c->page))return ROS_FAILED;
  ++c->index;return ROS_PENDING;}
 if(stage==5){if(c->index==s->pages){c->status.stage=8;return ROS_PENDING;}
  if(read_page(c,sector(c,s->slot)+1U+c->index)!=ROS_READ_OK)return fault(c,ROS_FAILED);
  size_t offset=(size_t)c->index*2048U,n=s->container_bytes-offset;if(n>2048U)n=2048U;
  if(memcmp(c->page,c->container+offset,n)||!fill(c->page+n,2048U-n,255))return fault(c,ROS_FAILED);
  ++c->index;return ROS_PENDING;}
 if(stage==6){if(write_page(c,sector(c,s->slot),c->catalog))return ROS_FAILED;++c->status.stage;return ROS_PENDING;}
 if(stage==7){if(sync_map(c))return ROS_FAILED;c->index=0;c->status.stage=5;return ROS_PENDING;}
 if(read_page(c,sector(c,s->slot))!=ROS_READ_OK||memcmp(c->page,c->catalog,2048))return fault(c,ROS_FAILED);
 set_slot_state(c,s->slot,SLOT_COMMIT);account(c);memcpy(c->receipt.es_header,c->container,128);c->receipt.durable=1;return ready(c);
}
int ros_step(struct ros_context *c)
{
 int rc=lock(c);if(rc)return rc;if(c->status.fault||c->status.job==ROS_NONE)return unlock(c,ROS_STATE);
 if(c->status.ready)return unlock(c,ROS_READY);
 if(guard(c))return unlock(c,ROS_FAILED);
 switch(c->status.job){case ROS_RECOVER:rc=recover_step(c);break;
 case ROS_RESERVE:case ROS_FINALIZE:rc=root_step(c);break;case ROS_SEGMENT:rc=segment_step(c);break;
 case ROS_META:case ROS_LOAD:if(c->status.stage==0){uint32_t kind;if(read_page(c,sector(c,c->slot))!=ROS_READ_OK||
   parse_segment(c,c->slot,&kind)||kind!=SLOT_COMMIT){rc=fault(c,ROS_FAILED);break;}
   c->index=0;c->status.stage=1;rc=c->status.job==ROS_META?ready(c):ROS_PENDING;}
  else if(c->index<c->segment.pages)rc=read_container_page(c);else rc=container_valid(c)?ROS_FAILED:ready(c);
  break;
 default:rc=fault(c,ROS_FAILED);break;}
 return unlock(c,rc);
}
static void finish_job(struct ros_context *c)
{buffers(c);wipe(&c->receipt,sizeof(c->receipt));wipe(&c->final_receipt,sizeof(c->final_receipt));
 c->status.job=ROS_NONE;c->status.stage=0;c->status.ready=0;}
int ros_take_segment_receipt(struct ros_context *c,struct rp_segment_receipt *out)
{int rc=lock(c);if(rc)return rc;if(!external(c,out,sizeof(*out)))return unlock(c,ROS_ARGUMENT);
 if(c->status.fault||!c->status.ready||c->status.job!=ROS_SEGMENT)return unlock(c,ROS_STATE);
 *out=c->receipt;finish_job(c);return unlock(c,ROS_OK);}
int ros_take_final_receipt(struct ros_context *c,struct rp_final_receipt *out)
{int rc=lock(c);if(rc)return rc;if(!external(c,out,sizeof(*out)))return unlock(c,ROS_ARGUMENT);
 if(c->status.fault||!c->status.ready||c->status.job!=ROS_FINALIZE)return unlock(c,ROS_STATE);
 *out=c->final_receipt;finish_job(c);return unlock(c,ROS_OK);}
int ros_finish(struct ros_context *c)
{int rc=lock(c);if(rc)return rc;if(c->status.fault||!c->status.ready||
 (c->status.job!=ROS_RECOVER&&c->status.job!=ROS_RESERVE&&c->status.job!=ROS_LOAD&&c->status.job!=ROS_META))return unlock(c,ROS_STATE);
 finish_job(c);return unlock(c,ROS_OK);}
int ros_copy_loaded(struct ros_context *c,uint32_t offset,uint8_t *out,size_t capacity,size_t *actual)
{int rc=lock(c);if(rc)return rc;if(!external(c,out,capacity)||!external(c,actual,sizeof(*actual))||
 capacity>2048||overlap(out,capacity,actual,sizeof(*actual)))return unlock(c,ROS_ARGUMENT);
 if(c->status.fault||!c->status.ready||c->status.job!=ROS_LOAD||offset>c->segment.container_bytes)return unlock(c,ROS_STATE);
 size_t n=c->segment.container_bytes-offset;if(n>capacity)n=capacity;memcpy(out,c->container+offset,n);*actual=n;return unlock(c,ROS_OK);}
int ros_get_status(struct ros_context *c,struct ros_status *out)
{int rc=lock(c);if(rc)return rc;if(!external(c,out,sizeof(*out)))return unlock(c,ROS_ARGUMENT);*out=c->status;return unlock(c,ROS_OK);}
int ros_get_geometry(struct ros_context *c,struct ros_geometry *out)
{int rc=lock(c);if(rc)return rc;if(!external(c,out,sizeof(*out)))return unlock(c,ROS_ARGUMENT);
 if(c->status.fault)return unlock(c,ROS_STATE);
 *out=(struct ros_geometry){c->root_count,c->status.slot_count,c->extent_profile};return unlock(c,ROS_OK);}
int ros_get_loaded_segment(struct ros_context *c,struct ros_segment *out)
{int rc=lock(c);if(rc)return rc;if(!external(c,out,sizeof(*out)))return unlock(c,ROS_ARGUMENT);
 if(c->status.fault||!c->status.ready||(c->status.job!=ROS_LOAD&&c->status.job!=ROS_META))return unlock(c,ROS_STATE);
 *out=c->segment;return unlock(c,ROS_OK);}
int ros_get_record(struct ros_context *c,uint32_t root,struct ros_record *out)
{int rc=lock(c);if(rc)return rc;if(!external(c,out,sizeof(*out))||root>=c->root_count)return unlock(c,ROS_ARGUMENT);
 if(!c->status.mounted||c->status.fault||!c->records[root].present)return unlock(c,ROS_NOT_FOUND);
 *out=c->records[root];return unlock(c,ROS_OK);}
