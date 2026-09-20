/* SPDX-License-Identifier: Apache-2.0 */
#include "recording_catalog.h"
#include <limits.h>
#include <string.h>
#define RCAT_MAGIC UINT32_C(0x52434131)
static int full_open(struct recording_catalog*,const uint8_t*,uint64_t);
static int full_handle(struct recording_catalog*,const struct db_request*,uint8_t*,size_t*,uint64_t);
static int full_approve_receipt(const struct recording_catalog*,const struct rsm_receipt*);
static int full_approve_delete(const struct recording_catalog*,const struct rsm_terminal*);
static uint32_t le32(const uint8_t *p){return (uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;}
static uint64_t le64(const uint8_t *p){return (uint64_t)le32(p)|(uint64_t)le32(p+4)<<32;}
static int valid_bytes(const uint8_t *p,size_t n)
{uint8_t a=0,b=0;while(n--){a|=*p;b|=(uint8_t)(*p++^255U);}return a&&b;}
static int separate(const void *a,size_t an,const void *b,size_t bn)
{uintptr_t x=(uintptr_t)a,y=(uintptr_t)b;return a&&b&&an&&bn&&an<=UINTPTR_MAX-x&&bn<=UINTPTR_MAX-y&&(x+an<=y||y+bn<=x);}
static int external(const struct recording_catalog *c,const void *p,size_t n)
{return separate(c,sizeof(*c),p,n)&&separate(c->store,sizeof(*c->store),p,n)&&separate(c->metadata,sizeof(*c->metadata),p,n);}
static void wipe(void *p,size_t n){volatile uint8_t *b=p;while(n--)*b++=0;}
static int fail(struct recording_catalog *c){c->fault=1;return RCAT_FAULT;}
static int check(struct recording_catalog *c)
{
 uint64_t now=c->port.now_ms(c->port.user);
 if(c->fault||atomic_load(&c->cancelled)||now<c->last_now||now>=c->deadline||
    (c->opened&&now>=c->expires))return fail(c);
 c->last_now=now;
 if(c->port.admit(c->port.user,c->deadline))return fail(c);
 now=c->port.now_ms(c->port.user);
 if(atomic_load(&c->cancelled)||now<c->last_now||now>=c->deadline||(c->opened&&now>=c->expires))return fail(c);
 c->last_now=now;return 0;
}
static int begin(struct recording_catalog *c,uint64_t deadline)
{
 if(!c||c->initialized!=RCAT_MAGIC)return RCAT_ARGUMENT;
 unsigned expected=0;if(!atomic_compare_exchange_strong(&c->gate,&expected,1))return RCAT_BUSY;
 uint64_t now=c->port.now_ms(c->port.user);
 uint64_t limit=c->full_profile&&!c->opened?300000U:30000U;
 c->deadline=deadline>now&&deadline-now>limit?now+limit:deadline;
 if(c->opened&&c->deadline>c->expires)c->deadline=c->expires;
 if(check(c)){atomic_store(&c->gate,0);return RCAT_FAULT;}return 0;
}
static int end(struct recording_catalog *c,int result)
{if(check(c))result=RCAT_FAULT;atomic_store(&c->gate,0);return result;}
static int finish_job(struct recording_catalog *c,int rc)
{
 while(rc==ROS_PENDING){
  if(check(c)||c->port.yield(c->port.user,c->deadline)||check(c))return fail(c);
  rc=ros_step(c->store);
 }
 return rc==ROS_READY&&!check(c)?0:fail(c);
}
static int binding(const struct recording_catalog *c,const struct es_binding *b)
{return !memcmp(c->volume.device,b->device_id,16)&&!memcmp(c->volume.volume,b->volume_id,16)&&
 c->volume.generation==b->generation&&!memcmp(c->volume.fingerprint,b->key_fingerprint,32);}
int rcat_init(struct recording_catalog *c,struct ros_context *store,struct recording_sync_metadata *metadata,
 const struct db_volume *volume,const struct owned_page_hash *hash,const struct rcat_port *port)
{
 if(!c||c->initialized||!store||!metadata||!volume||!hash||!hash->sha256||!port||!port->now_ms||!port->admit||!port->yield||!port->retire_admit||
 !valid_bytes(volume->device,16)||!valid_bytes(volume->volume,16)||!valid_bytes(volume->fingerprint,32)||
 !volume->generation||volume->generation>INT64_MAX||!atomic_is_lock_free(&c->gate))return RCAT_ARGUMENT;
 const void *spans[]={c,store,metadata,volume,hash,port};
 const size_t sizes[]={sizeof(*c),sizeof(*store),sizeof(*metadata),sizeof(*volume),sizeof(*hash),sizeof(*port)};
 for(unsigned i=0;i<6;++i)for(unsigned j=i+1;j<6;++j)if(!separate(spans[i],sizes[i],spans[j],sizes[j]))return RCAT_ARGUMENT;
 c->store=store;c->metadata=metadata;c->volume=*volume;c->hash=*hash;c->port=*port;c->initialized=RCAT_MAGIC;return 0;
}
static int insert(struct recording_catalog *c,const struct ros_record *record,const struct ros_segment *segments,
 size_t count,const uint8_t slot_list[32])
{
 uint32_t index=0;while(index<c->count&&memcmp(c->items[index].entry.recording,record->identity.recording_id,16)<0)++index;
 if(c->count==DB_MAX_RECORDINGS||(index<c->count&&!memcmp(c->items[index].entry.recording,record->identity.recording_id,16)))return fail(c);
 memmove(c->items+index+1,c->items+index,(c->count-index)*sizeof(c->items[0]));
 memmove(c->slots+index+1,c->slots+index,(c->count-index)*sizeof(c->slots[0]));
 struct rcat_item *item=&c->items[index];memset(item,0,sizeof(*item));
 item->bytes=(uint16_t)(RM_HEADER_BYTES+count*RM_ENTRY_BYTES);
 if(recording_manifest_build(item->manifest,item->bytes,item->entry.manifest_sha256,record,segments,count,&c->hash))return fail(c);
 memcpy(item->entry.recording,record->identity.recording_id,16);item->entry.revision=le64(item->manifest+72);
 item->entry.state=item->manifest[14];item->entry.segments=(uint32_t)count;
 memcpy(c->slots[index],slot_list,32);++c->count;return check(c);
}
int rcat_open(struct recording_catalog *c,const uint8_t nonce[16],uint64_t deadline)
{
 if(!c||c->initialized!=RCAT_MAGIC||!external(c,nonce,16)||!valid_bytes(nonce,16))return RCAT_ARGUMENT;
 if(c->full_profile)return full_open(c,nonce,deadline);
 int rc=begin(c,deadline);if(rc)return rc;
 if(c->opened)return end(c,RCAT_REFUSED);
 if(c->prior_valid&&!memcmp(nonce,c->prior_nonce,16))return end(c,RCAT_REFUSED);
 if(c->last_now>UINT64_MAX-900000U)return end(c,fail(c));
 struct ros_status status;if(ros_get_status(c->store,&status)||!status.mounted||status.fault||status.job||status.slot_count>ROS_SLOTS)return end(c,fail(c));
 for(uint32_t root=0;root<ROS_ROOTS;++root){
  struct ros_record record;uint8_t slot_list[32];memset(slot_list,255,sizeof(slot_list));
  memset(c->segments,0,sizeof(c->segments));
  if(check(c))return end(c,fail(c));
  rc=ros_get_record(c->store,root,&record);
  if(rc==ROS_NOT_FOUND)continue;
  if(rc||!record.present)return end(c,fail(c));
  if(!binding(c,&record.identity))return end(c,fail(c));
  int deleted=rsm_record_deleted(c->metadata,record.identity.recording_id);
  if(deleted==RSM_OK)continue;
  if(deleted!=RSM_NOT_FOUND)return end(c,fail(c));
  uint32_t found=0;
  for(uint32_t slot=0;slot<status.slot_count;++slot){
   rc=ros_describe_committed(c->store,slot,c->deadline);
   if(rc==ROS_NOT_FOUND)continue;
   if(finish_job(c,rc))return end(c,fail(c));
   struct ros_segment segment;
   if(ros_get_loaded_segment(c->store,&segment)||ros_finish(c->store)||!binding(c,&segment.identity))return end(c,fail(c));
   if(memcmp(segment.identity.recording_id,record.identity.recording_id,16))continue;
   uint32_t sequence=segment.identity.segment_sequence;
   if(sequence>=32||slot_list[sequence]!=255||segment.root!=root||segment.slot!=slot)return end(c,fail(c));
   c->segments[sequence]=segment;slot_list[sequence]=(uint8_t)slot;++found;
  }
  if(found!=record.segments||found>32)return end(c,fail(c));
  for(uint32_t i=0;i<found;++i)if(slot_list[i]==255)return end(c,fail(c));
  if(insert(c,&record,c->segments,found,slot_list))return end(c,fail(c));
 }
 struct db_catalog_entry entries[DB_MAX_RECORDINGS];
 for(uint32_t i=0;i<c->count;++i)entries[i]=c->items[i].entry;
 c->catalog_bytes=(uint16_t)(DB_CATALOG_HEADER+c->count*DB_CATALOG_ENTRY);
 if(durable_ble_catalog_build(c->catalog,c->catalog_bytes,&c->volume,nonce,1,entries,c->count))return end(c,fail(c));
 if(c->last_now>UINT64_MAX-900000U)return end(c,fail(c));
 memcpy(c->nonce,nonce,16);c->expires=c->last_now+900000U;c->opened=1;return end(c,0);
}
static int item_index(const struct recording_catalog *c,const uint8_t recording[16])
{for(uint32_t i=0;i<c->count;++i)if(!memcmp(c->items[i].entry.recording,recording,16))return (int)i;return -1;}
static const uint8_t *entry(const struct recording_catalog *c,int item,uint32_t sequence)
{return item>=0&&sequence<c->items[item].entry.segments?c->items[item].manifest+RM_HEADER_BYTES+sequence*RM_ENTRY_BYTES:NULL;}
static int volume_match(const struct recording_catalog *c,const struct rsm_volume *v)
{return v&&!memcmp(v->device,c->volume.device,16)&&!memcmp(v->volume,c->volume.volume,16)&&
 !memcmp(v->recipient,c->volume.fingerprint,32)&&v->generation==c->volume.generation;}
int rcat_approve_receipt(const struct recording_catalog *c,const struct rsm_volume *v,const struct rsm_receipt *r)
{
 if(!c||c->initialized!=RCAT_MAGIC||!c->opened||c->fault||atomic_load(&c->cancelled)||!r||!volume_match(c,v))return -1;
 if(c->full_profile)return full_approve_receipt(c,r);
 const uint8_t *e=entry(c,item_index(c,r->recording),r->sequence);
 return e&&le32(e)==r->sequence&&le32(e+4)==r->container_bytes&&!memcmp(e+32,r->digest,32)?0:-1;
}
int rcat_approve_delete(const struct recording_catalog *c,const struct rsm_volume *v,const uint8_t operation[16],const struct rsm_terminal *t)
{
 if(!c||c->initialized!=RCAT_MAGIC||!c->opened||c->fault||atomic_load(&c->cancelled)||!t||!operation||!valid_bytes(operation,16)||!volume_match(c,v))return -1;
 if(c->full_profile)return full_approve_delete(c,t);
 int i=item_index(c,t->recording);if(i<0)return -1;const struct db_catalog_entry *e=&c->items[i].entry;
 return e->state&&e->state==t->state&&e->segments==t->segments&&e->revision==t->revision&&!memcmp(e->manifest_sha256,t->manifest,32)?0:-1;
}
/* Reuse the frozen codec's closed typed-request grammar BEFORE any operation
 * with persistence effects. These dummy frames never leave this function. */
static int request_valid(const struct db_request *r)
{
 uint8_t frame[DB_MAX_RESPONSE],data[DB_MAX_DATA]={0};
 if(r->command==DB_RECEIVE_ACK)return !durable_ble_receipt_response(frame,81,r);
 if(r->command==DB_DELETE)return !durable_ble_delete_response(frame,81,r,1,2);
 uint32_t total;
 if(r->command==DB_CATALOG)total=DB_MAX_CATALOG;
 else if(r->command==DB_MANIFEST){if(r->revision>130U)return 0;total=128U+64U*(uint32_t)(r->revision/4U);}
 else if(r->command==DB_SEGMENT)total=DB_MAX_CONTAINER;
 else return 0;
 if(!r->maximum||r->maximum>DB_MAX_DATA||r->offset>=total)return 0;
 size_t n=total-r->offset;if(n>r->maximum)n=r->maximum;
 return !durable_ble_read_response(frame,29+n,r,(uint16_t)total,data,n);
}
int rcat_handle(struct recording_catalog *c,const struct db_request *request,uint8_t response[DB_MAX_RESPONSE],size_t *actual,uint64_t deadline)
{
 if(!c||c->initialized!=RCAT_MAGIC||!external(c,request,sizeof(*request))||!external(c,response,DB_MAX_RESPONSE)||
 !external(c,actual,sizeof(*actual))||!separate(request,sizeof(*request),response,DB_MAX_RESPONSE)||
 !separate(request,sizeof(*request),actual,sizeof(*actual))||!separate(response,DB_MAX_RESPONSE,actual,sizeof(*actual)))return RCAT_ARGUMENT;
 if(c->full_profile)return full_handle(c,request,response,actual,deadline);
 if(!request_valid(request))return RCAT_REFUSED;
 int rc=begin(c,deadline);if(rc)return rc;
 if(!c->opened||memcmp(request->nonce,c->nonce,16))return end(c,RCAT_REFUSED);
 /* The frozen catalog remains byte-identical, but a durable tombstone revokes
  * later data/receipt selectors even in that same snapshot. Exact DELETE replay
  * remains available independently of catalog membership. */
 if(request->command==DB_MANIFEST||request->command==DB_SEGMENT||request->command==DB_RECEIVE_ACK){
  rc=rsm_record_deleted(c->metadata,request->recording);
  if(rc==RSM_OK)return end(c,RCAT_REFUSED);
  if(rc!=RSM_NOT_FOUND)return end(c,fail(c));
 }
 int i=item_index(c,request->recording);const uint8_t *data=NULL;uint32_t total=0;
 uint8_t bytes[DB_MAX_DATA],frame[DB_MAX_RESPONSE];size_t size=0;
 if(request->command==DB_CATALOG){data=c->catalog;total=c->catalog_bytes;}
 else if(request->command==DB_MANIFEST){
  if(i<0||request->revision!=c->items[i].entry.revision||memcmp(request->sha256,c->items[i].entry.manifest_sha256,32))return end(c,RCAT_REFUSED);
  data=c->items[i].manifest;total=c->items[i].bytes;
 }else if(request->command==DB_SEGMENT||request->command==DB_RECEIVE_ACK){
  const uint8_t *e=entry(c,i,request->segment);
  if(!e||memcmp(e+32,request->sha256,32))return end(c,RCAT_REFUSED);
  total=le32(e+4);
  if(request->command==DB_RECEIVE_ACK){
   if(request->container_bytes!=total)return end(c,RCAT_REFUSED);
   struct rsm_receipt receipt={.sequence=request->segment,.container_bytes=total};
   memcpy(receipt.recording,request->recording,16);memcpy(receipt.digest,request->sha256,32);
   if(rsm_receive(c->metadata,&receipt,c->deadline)||check(c))return end(c,fail(c));
   size=81;if(durable_ble_receipt_response(frame,size,request))return end(c,fail(c));
  }else{
   if(!request->maximum||request->maximum>DB_MAX_DATA||request->offset>=total)return end(c,RCAT_REFUSED);
   uint32_t slot=c->slots[i][request->segment];
   if(c->loaded&&slot!=c->loaded_slot){if(ros_finish(c->store))return end(c,fail(c));c->loaded=0;}
   if(!c->loaded){
    if(finish_job(c,ros_load(c->store,slot,c->deadline)))return end(c,fail(c));
    struct ros_segment loaded;
    if(ros_get_loaded_segment(c->store,&loaded)||!binding(c,&loaded.identity)||
     memcmp(loaded.identity.recording_id,request->recording,16)||loaded.identity.segment_sequence!=request->segment||
     loaded.container_bytes!=total||memcmp(loaded.digest,request->sha256,32))return end(c,fail(c));
    c->loaded=1;c->loaded_slot=slot;
   }
   size_t n=total-request->offset;if(n>request->maximum)n=request->maximum;
   size_t got=0;if(ros_copy_loaded(c->store,request->offset,bytes,n,&got)||got!=n)return end(c,fail(c));
   size=29+n;if(durable_ble_read_response(frame,size,request,(uint16_t)total,bytes,n))return end(c,fail(c));
  }
 }else if(request->command==DB_DELETE){
  struct rsm_tombstone saved;rc=rsm_replay_delete(c->metadata,request->operation,request->sha256,&saved);
  uint64_t revision=0,original=0;
  if(rc==RSM_OK){revision=saved.revision;original=saved.terminal.revision;}
  else if(rc==RSM_NOT_FOUND){
   const struct db_catalog_entry *e=NULL;
   for(uint32_t k=0;k<c->count;++k)if(!memcmp(c->items[k].entry.manifest_sha256,request->sha256,32)){
    if(e)return end(c,fail(c));
    e=&c->items[k].entry;
   }
   if(!e||!e->state)return end(c,RCAT_REFUSED);
   struct rsm_terminal terminal={.revision=e->revision,.segments=e->segments,.state=e->state};
   memcpy(terminal.recording,e->recording,16);memcpy(terminal.manifest,e->manifest_sha256,32);original=e->revision;
   if(rsm_delete(c->metadata,request->operation,&terminal,&revision,c->deadline)||check(c))return end(c,fail(c));
  }else return end(c,fail(c));
  size=81;if(durable_ble_delete_response(frame,size,request,original,revision))return end(c,fail(c));
 }else return end(c,RCAT_REFUSED);
 if(data){
  if(!request->maximum||request->maximum>DB_MAX_DATA||request->offset>=total)return end(c,RCAT_REFUSED);
  size_t n=total-request->offset;if(n>request->maximum)n=request->maximum;
  size=29+n;if(durable_ble_read_response(frame,size,request,(uint16_t)total,data+request->offset,n))return end(c,fail(c));
 }
 if(!size||check(c))return end(c,fail(c));
 /* No callback after publication: a late final guard must not return a valid
  * success-shaped frame with a failure status. Parent still checks its lease
  * immediately before queuing these bytes to the transport. */
 memcpy(response,frame,size);*actual=size;atomic_store(&c->gate,0);return RCAT_OK;
}
int rcat_cancel(struct recording_catalog *c)
{if(!c||c->initialized!=RCAT_MAGIC)return RCAT_ARGUMENT;atomic_store(&c->cancelled,1);return 0;}
int rcat_retire(struct recording_catalog *c,uint64_t deadline)
{
 if(!c||c->initialized!=RCAT_MAGIC)return RCAT_ARGUMENT;
 unsigned expected=0;if(!atomic_compare_exchange_strong(&c->gate,&expected,1))return RCAT_BUSY;
 int rc=RCAT_REFUSED;uint64_t now=c->port.now_ms(c->port.user);
 if(c->fault||!atomic_load(&c->cancelled)||now<c->last_now||deadline<=now||deadline-now>30000U)goto out;
 c->last_now=now;
 if(c->port.retire_admit(c->port.user,deadline))goto out;
 now=c->port.now_ms(c->port.user);
 if(now<c->last_now||now>=deadline){rc=fail(c);goto out;}
 c->last_now=now;
 struct ros_status status;struct rsm_root_proof proof;
 if(ros_get_status(c->store,&status)||status.fault||!status.mounted||rsm_get_root_proof(c->metadata,&proof)){rc=fail(c);goto out;}
 if(c->loaded){
  if(status.job!=ROS_LOAD||!status.ready||ros_finish(c->store)){rc=fail(c);goto out;}
 }else if(status.job){rc=fail(c);goto out;}
 now=c->port.now_ms(c->port.user);
 if(now<c->last_now||now>=deadline){rc=fail(c);goto out;}
 c->last_now=now;
 if(c->opened){memcpy(c->prior_nonce,c->nonce,16);c->prior_valid=1;}
 wipe(c->nonce,sizeof(c->nonce));wipe(c->catalog,sizeof(c->catalog));wipe(c->items,sizeof(c->items));
 wipe(c->slots,sizeof(c->slots));wipe(c->segments,sizeof(c->segments));
 if(c->full_profile)wipe(&c->full,sizeof(c->full));
 c->opened=c->count=c->loaded=c->loaded_slot=c->catalog_bytes=0;c->expires=c->deadline=0;
 atomic_store(&c->cancelled,0);rc=RCAT_OK;
out: atomic_store(&c->gate,0);return rc;
}
#include "recording_catalog_full.inc"
