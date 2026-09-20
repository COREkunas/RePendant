/* SPDX-License-Identifier: Apache-2.0
 * Full-chip immutable catalog. Included only by recording_catalog.c.
 * One u16 physical slot per segment, NOT a retained manifest per recording.
 * The parent owns ROS/RSM exclusively for this entire snapshot, including
 * while serving fragments. Storage mutation outside this actor is forbidden.
 */
int rcat_init_full(struct recording_catalog *c,struct ros_context *store,
 struct recording_sync_metadata *metadata,const struct db_volume *volume,
 const struct owned_page_hash *hash,const struct rcat_port *port,const struct rm_stream_port *stream)
{
 if(!c||!store||!metadata||!stream||!stream->begin||!stream->update||!stream->finish||!stream->abort||
  !separate(c,sizeof(*c),stream,sizeof(*stream))||!separate(store,sizeof(*store),stream,sizeof(*stream))||
  !separate(metadata,sizeof(*metadata),stream,sizeof(*stream)))return RCAT_ARGUMENT;
 struct rsm_root_proof proof;struct ros_status status;
 if(rsm_get_root_proof(metadata,&proof)||proof.capacity!=RLL_CAPACITY||proof.ros_capacity!=RLL_ROS_CAPACITY||
  proof.slots!=RLL_SLOTS||ros_get_status(store,&status)||!status.mounted||status.fault||status.job||
  status.slot_count!=RLL_SLOTS)return RCAT_REFUSED;
 int rc=rcat_init(c,store,metadata,volume,hash,port);if(rc)return rc;
 c->streaming=*stream;c->full_profile=1;return 0;
}
static uint64_t full_job_deadline(struct recording_catalog *c)
{uint64_t n=c->port.now_ms(c->port.user);return c->deadline>n&&c->deadline-n>ROS_JOB_MAX_MS?n+ROS_JOB_MAX_MS:c->deadline;}
static int full_release_loaded(struct recording_catalog *c)
{if(c->loaded){if(ros_finish(c->store))return fail(c);c->loaded=0;}return check(c);}
static int full_describe(struct recording_catalog *c,uint32_t slot,struct ros_segment *out)
{
 if(full_release_loaded(c))return RCAT_FAULT;
 int rc=ros_describe_committed(c->store,slot,full_job_deadline(c));
 if(rc==ROS_NOT_FOUND)return rc;
 if(finish_job(c,rc)||ros_get_loaded_segment(c->store,out)||ros_finish(c->store)||!binding(c,&out->identity)||
  out->slot!=slot)return fail(c);
 return check(c);
}
static int full_index(const struct recording_catalog *c,const uint8_t id[16])
{for(uint32_t i=0;i<c->count;++i)if(!memcmp(id,c->full.items[i].entry.recording,16))return (int)i;return -1;}
static int full_segment(struct recording_catalog *c,uint32_t item,uint32_t seq,struct ros_segment *out)
{
 if(item>=c->count||seq>=c->full.items[item].entry.segments)return RCAT_REFUSED;
 uint32_t slot=c->full.slots[c->full.items[item].base+seq];
 if(slot>=RLL_SLOTS||full_describe(c,slot,out)||out->root!=c->full.items[item].root||
  out->identity.segment_sequence!=seq||memcmp(out->identity.recording_id,c->full.items[item].entry.recording,16))return fail(c);
 return 0;
}
static int full_stream_begin(void *u)
{struct recording_catalog *c=u;return check(c)||c->streaming.begin(c->streaming.user)||check(c)?-1:0;}
static int full_stream_update(void *u,const uint8_t *p,size_t n)
{struct recording_catalog *c=u;return check(c)||c->streaming.update(c->streaming.user,p,n)||check(c)?-1:0;}
static int full_stream_finish(void *u,uint8_t sum[32])
{struct recording_catalog *c=u;return check(c)||c->streaming.finish(c->streaming.user,sum)||check(c)?-1:0;}
static void full_stream_abort(void *u)
{struct recording_catalog *c=u;c->streaming.abort(c->streaming.user);}
static int full_stream_segment(void *u,uint32_t seq,struct ros_segment *s)
{struct recording_catalog *c=u;return full_segment(c,c->full.selected,seq,s);}
static int full_run_clear(struct recording_catalog *c)
{
 /* Abort even a failed begin, as required by the provider contract. The
  * runtime fences admission if abort itself fails. Always scrub staging. */
 if(c->full.hash_active){full_stream_abort(c);c->full.hash_active=0;}
 recording_manifest_clear(&c->full.run);c->full.run_item=0;return check(c);
}
static int full_run_segment(struct recording_catalog *c,uint32_t item,const struct ros_segment *s)
{
 /* Physical order is NOT a storage invariant. A different root or sequence
  * only abandons this speculative hash; the unchanged whole-stream fallback
  * will validate that root. Integrity/provider/admission errors never fall back. */
 if(c->full.hash_active&&(c->full.run_item!=item||c->full.run.count!=s->identity.segment_sequence))
  if(full_run_clear(c))return fail(c);
 if(!c->full.hash_active){
  if(s->identity.segment_sequence||c->full.ready[item])return check(c);
  struct ros_record r;
  if(ros_get_record(c->store,c->full.items[item].root,&r)||
   recording_manifest_start(&c->full.run,&r,c->full.items[item].header))return fail(c);
  c->full.run_item=item;c->full.hash_active=1;
  if(full_stream_begin(c)||full_stream_update(c,c->full.items[item].header,128))return fail(c);
 }
 if(recording_manifest_push(&c->full.run,s,c->full.entry)||full_stream_update(c,c->full.entry,64))return fail(c);
 if(c->full.run.count==c->full.run.record.segments){
  if(recording_manifest_finish(&c->full.run)||full_stream_finish(c,c->full.items[item].entry.manifest_sha256)||
   full_run_clear(c))return fail(c);
  c->full.ready[item]=1;
 }
 return check(c);
}
static int full_open_failed(struct recording_catalog *c)
{
 (void)full_run_clear(c);memset(c->full.ready,0,sizeof(c->full.ready));
 for(uint32_t i=0;i<RLL_ROOTS;++i){wipe(c->full.items[i].header,128);wipe(c->full.items[i].entry.manifest_sha256,32);}
 wipe(c->full.entry,sizeof(c->full.entry));wipe(c->catalog,sizeof(c->catalog));c->catalog_bytes=0;c->opened=0;
 return end(c,fail(c));
}
static int full_open(struct recording_catalog *c,const uint8_t *nonce,uint64_t deadline)
{
 int rc=begin(c,deadline);if(rc)return rc;
 if(c->opened||(c->prior_valid&&!memcmp(nonce,c->prior_nonce,16)))return end(c,RCAT_REFUSED);
 struct ros_status status;
 if(ros_get_status(c->store,&status)||!status.mounted||status.fault||status.job||status.slot_count!=RLL_SLOTS)return full_open_failed(c);
 memset(c->full.slots,255,sizeof(c->full.slots));
 /* Root metadata is already validated by read-only ROS recovery. */
 for(uint32_t root=0;root<RLL_ROOTS;++root){struct ros_record r;
  if(check(c))return full_open_failed(c);
  rc=ros_get_record(c->store,root,&r);if(rc==ROS_NOT_FOUND)continue;
  if(rc||!r.present||!binding(c,&r.identity)||r.segments>RLL_SLOTS)return full_open_failed(c);
  rc=rsm_record_deleted(c->metadata,r.identity.recording_id);if(rc==RSM_OK)continue;
  if(rc!=RSM_NOT_FOUND||c->count>=RLL_ROOTS)return full_open_failed(c);
  uint32_t i=0;while(i<c->count&&memcmp(c->full.items[i].entry.recording,r.identity.recording_id,16)<0)++i;
  if(i<c->count&&!memcmp(c->full.items[i].entry.recording,r.identity.recording_id,16))return full_open_failed(c);
  memmove(c->full.items+i+1,c->full.items+i,(c->count-i)*sizeof(c->full.items[0]));
  memset(&c->full.items[i],0,sizeof(c->full.items[0]));
  memcpy(c->full.items[i].entry.recording,r.identity.recording_id,16);
  c->full.items[i].entry.segments=r.segments;c->full.items[i].root=(uint16_t)root;++c->count;
 }
 uint32_t total=0;
 for(uint32_t i=0;i<c->count;++i){
  c->full.items[i].base=(uint16_t)total;
  if(c->full.items[i].entry.segments>RLL_SLOTS-total)return full_open_failed(c);
  total+=c->full.items[i].entry.segments;
 }
 /* Single scan over the physical slot index, independent of root count. */
 uint32_t found=0;
 for(uint32_t slot=0;slot<RLL_SLOTS;++slot){struct ros_segment s;
  rc=full_describe(c,slot,&s);if(rc==ROS_NOT_FOUND)continue;
  if(rc)return full_open_failed(c);
  int i=full_index(c,s.identity.recording_id);
  if(i<0){if(rsm_record_deleted(c->metadata,s.identity.recording_id)==RSM_OK)continue;return full_open_failed(c);}
  uint32_t seq=s.identity.segment_sequence;
  if(seq>=c->full.items[i].entry.segments||s.root!=c->full.items[i].root)return full_open_failed(c);
  uint32_t pos=c->full.items[i].base+seq;
  if(c->full.slots[pos]!=UINT16_MAX)return full_open_failed(c);
  c->full.slots[pos]=(uint16_t)slot;++found;
  if(full_run_segment(c,(uint32_t)i,&s))return full_open_failed(c);
 }
 if(found!=total||full_run_clear(c))return full_open_failed(c);
 struct rm_stream_port stream={c,full_stream_begin,full_stream_update,full_stream_finish,full_stream_abort,full_stream_segment};
 struct db_catalog_entry entries[RLL_ROOTS];
 for(uint32_t i=0;i<c->count;++i){struct ros_record r;
  c->full.selected=i;
  if(!c->full.ready[i]&&(ros_get_record(c->store,c->full.items[i].root,&r)||
   recording_manifest_stream(c->full.items[i].header,c->full.items[i].entry.manifest_sha256,&r,&stream)))return full_open_failed(c);
  c->full.items[i].entry.revision=le64(c->full.items[i].header+72);
  c->full.items[i].entry.state=c->full.items[i].header[14];entries[i]=c->full.items[i].entry;
 }
 c->catalog_bytes=(uint16_t)(128+c->count*64);
 if(durable_ble_catalog_build_full(c->catalog,c->catalog_bytes,&c->volume,nonce,1,entries,c->count)||check(c)||
  c->last_now>UINT64_MAX-900000U)return full_open_failed(c);
 /* Final admission before publication, still under the gate. There are no
  * callbacks after this guard and before publishing the immutable snapshot. */
 if(check(c))return full_open_failed(c);
 memcpy(c->nonce,nonce,16);c->expires=c->last_now+900000U;c->opened=1;atomic_store(&c->gate,0);return 0;
}
static int full_cache(struct recording_catalog *c,uint32_t item,uint32_t seq)
{
 if(c->full.cache_valid&&c->full.cache_item==item&&c->full.cache_sequence==seq)return check(c);
 c->full.cache_valid=0;
 if(full_segment(c,item,seq,&c->full.segment)||recording_manifest_entry(c->full.entry,&c->full.segment))return fail(c);
 c->full.cache_item=item;c->full.cache_sequence=seq;c->full.cache_valid=1;return 0;
}
static int full_approve_receipt(const struct recording_catalog *c,const struct rsm_receipt *r)
{
 if(c->full.batch_count){
  if(!c->full.batch_receipts||c->full.batch_count>DB_RECEIPT_BATCH_MAX||!atomic_load(&c->gate))return -1;
  for(uint32_t i=0;i<c->full.batch_count;++i)
   if(!memcmp(c->full.batch_receipts+i,r,sizeof(*r)))return 0;
  return -1;
 }
 const struct ros_segment *s=&c->full.segment;
 return c->full.cache_valid&&!memcmp(s->identity.recording_id,r->recording,16)&&
  s->identity.segment_sequence==r->sequence&&s->container_bytes==r->container_bytes&&!memcmp(s->digest,r->digest,32)?0:-1;
}
static int full_approve_delete(const struct recording_catalog *c,const struct rsm_terminal *t)
{
 int i=full_index(c,t->recording);if(i<0)return -1;
 const struct db_catalog_entry *e=&c->full.items[i].entry;
 return e->state&&e->state==t->state&&e->segments==t->segments&&e->revision==t->revision&&!memcmp(e->manifest_sha256,t->manifest,32)?0:-1;
}
static int full_request_valid(const struct db_request *r)
{
 uint8_t frame[DB_MAX_RESPONSE],data[DB_FULL_MAX_DATA]={0};uint32_t total;
 if(r->command==DB_FULL_RECEIVE_ACK||r->command==DB_FULL_RECEIVE_RANGE)return !durable_ble_receipt_response(frame,81,r);
 if(r->command==DB_FULL_DELETE)return !durable_ble_delete_response(frame,81,r,1,2);
 if(r->command==DB_FULL_CATALOG)total=DB_FULL_MAX_CATALOG;
 else if(r->command==DB_FULL_MANIFEST){if(r->revision>RLL_SLOTS*4U+2U)return 0;total=128+64*(uint32_t)(r->revision/4);}
 else if(r->command==DB_FULL_SEGMENT)total=DB_MAX_CONTAINER;
 else return 0;
 if(!r->maximum||r->maximum>DB_FULL_MAX_DATA||r->offset>=total)return 0;
 size_t n=total-r->offset;if(n>r->maximum)n=r->maximum;
 return !durable_ble_read_response(frame,33+n,r,total,data,n);
}
static int full_handle(struct recording_catalog *c,const struct db_request *r,uint8_t *response,size_t *actual,uint64_t deadline)
{
 if(!full_request_valid(r))return RCAT_REFUSED;
 int rc=begin(c,deadline);if(rc)return rc;
 if(!c->opened||memcmp(r->nonce,c->nonce,16))return end(c,RCAT_REFUSED);
 if(r->command!=DB_FULL_CATALOG&&r->command!=DB_FULL_DELETE){
  rc=rsm_record_deleted(c->metadata,r->recording);
  if(rc==RSM_OK)return end(c,RCAT_REFUSED);
  if(rc!=RSM_NOT_FOUND)return end(c,fail(c));
 }
 uint8_t data[DB_FULL_MAX_DATA],frame[DB_MAX_RESPONSE];size_t size=0,n=0;uint32_t total=0;
 int i=full_index(c,r->recording);
 if(r->command==DB_FULL_CATALOG)total=c->catalog_bytes;
 else if(r->command==DB_FULL_MANIFEST){
  if(i<0||r->revision!=c->full.items[i].entry.revision||memcmp(r->sha256,c->full.items[i].entry.manifest_sha256,32))return end(c,RCAT_REFUSED);
  total=128+64*c->full.items[i].entry.segments;
 }else if(r->command==DB_FULL_RECEIVE_RANGE){
  if(i<0||!c->full.items[i].entry.state||memcmp(r->sha256,c->full.items[i].entry.manifest_sha256,32)||
   r->segment>=c->full.items[i].entry.segments||r->container_bytes>c->full.items[i].entry.segments-r->segment)
   return end(c,RCAT_REFUSED);
  uint32_t slots[DB_RECEIPT_BATCH_MAX];struct rsm_receipt receipts[DB_RECEIPT_BATCH_MAX];
  for(uint32_t j=0;j<r->container_bytes;++j){
   if(full_cache(c,(uint32_t)i,r->segment+j))return end(c,fail(c));
   const struct ros_segment *s=&c->full.segment;
   slots[j]=s->slot;memset(receipts+j,0,sizeof(receipts[j]));
   memcpy(receipts[j].recording,s->identity.recording_id,16);receipts[j].sequence=s->identity.segment_sequence;
   receipts[j].container_bytes=s->container_bytes;memcpy(receipts[j].digest,s->digest,32);
  }
  c->full.batch_receipts=receipts;c->full.batch_count=r->container_bytes;
  rc=rsm_receive_slots(c->metadata,slots,receipts,r->container_bytes,c->deadline);
  c->full.batch_receipts=NULL;c->full.batch_count=0;
  wipe(receipts,sizeof(receipts));wipe(slots,sizeof(slots));
  if(rc||check(c))return end(c,fail(c));
  size=81;if(durable_ble_receipt_response(frame,size,r))return end(c,fail(c));
 }else if(r->command==DB_FULL_SEGMENT||r->command==DB_FULL_RECEIVE_ACK){
  if(i<0||r->segment>=c->full.items[i].entry.segments)return end(c,RCAT_REFUSED);
  if(full_cache(c,(uint32_t)i,r->segment))return end(c,fail(c));
  const struct ros_segment *s=&c->full.segment;
  if(memcmp(s->digest,r->sha256,32))return end(c,RCAT_REFUSED);
  total=s->container_bytes;
  if(r->command==DB_FULL_RECEIVE_ACK){
   if(r->container_bytes!=total)return end(c,RCAT_REFUSED);
   struct rsm_receipt receipt={.sequence=r->segment,.container_bytes=total};
   memcpy(receipt.recording,r->recording,16);memcpy(receipt.digest,r->sha256,32);
   if(rsm_receive_slot(c->metadata,s->slot,&receipt,c->deadline)||check(c))return end(c,fail(c));
   size=81;if(durable_ble_receipt_response(frame,size,r))return end(c,fail(c));
  }else{
   if(c->loaded&&c->loaded_slot!=s->slot&&full_release_loaded(c))return end(c,fail(c));
   if(!c->loaded){struct ros_segment loaded;
    if(finish_job(c,ros_load(c->store,s->slot,full_job_deadline(c)))||ros_get_loaded_segment(c->store,&loaded)||
     !binding(c,&loaded.identity)||loaded.root!=s->root||loaded.slot!=s->slot||
     memcmp(loaded.identity.recording_id,r->recording,16)||loaded.identity.segment_sequence!=r->segment||
     loaded.container_bytes!=total||memcmp(loaded.digest,r->sha256,32))return end(c,fail(c));
    c->loaded=1;c->loaded_slot=s->slot;
   }
  }
 }else if(r->command==DB_FULL_DELETE){
  struct rsm_tombstone saved;uint64_t original=0,revision=0;
  rc=rsm_replay_delete_paged(c->metadata,r->operation,r->sha256,&saved,c->deadline);
  if(rc==RSM_OK){original=saved.terminal.revision;revision=saved.revision;}
  else if(rc==RSM_NOT_FOUND){const struct db_catalog_entry *e=NULL;
   for(uint32_t k=0;k<c->count;++k)if(!memcmp(c->full.items[k].entry.manifest_sha256,r->sha256,32)){
    if(e)return end(c,fail(c));e=&c->full.items[k].entry;
   }
   if(!e||!e->state)return end(c,RCAT_REFUSED);
   struct rsm_terminal t={.revision=e->revision,.segments=e->segments,.state=e->state};
   memcpy(t.recording,e->recording,16);memcpy(t.manifest,e->manifest_sha256,32);original=e->revision;
   if(rsm_delete(c->metadata,r->operation,&t,&revision,c->deadline)||check(c))return end(c,fail(c));
  }else return end(c,fail(c));
  size=81;if(durable_ble_delete_response(frame,size,r,original,revision))return end(c,fail(c));
 }
 if(!size){
  if(r->offset>=total)return end(c,RCAT_REFUSED);
  n=total-r->offset;if(n>r->maximum)n=r->maximum;
  if(r->command==DB_FULL_CATALOG)memcpy(data,c->catalog+r->offset,n);
  else if(r->command==DB_FULL_MANIFEST){
   for(size_t j=0;j<n;){uint32_t pos=r->offset+(uint32_t)j,available;const uint8_t *p;
    if(pos<128){p=c->full.items[i].header+pos;available=128-pos;}
    else{uint32_t seq=(pos-128)/64,off=(pos-128)%64;
     if(full_cache(c,(uint32_t)i,seq))return end(c,fail(c));
     p=c->full.entry+off;available=64-off;
    }
    size_t take=n-j;if(take>available)take=available;
    memcpy(data+j,p,take);j+=take;
   }
  }else{size_t got=0;if(ros_copy_loaded(c->store,r->offset,data,n,&got)||got!=n)return end(c,fail(c));}
  size=33+n;if(durable_ble_read_response(frame,size,r,total,data,n))return end(c,fail(c));
 }
 if(check(c))return end(c,fail(c));
 memcpy(response,frame,size);*actual=size;atomic_store(&c->gate,0);return RCAT_OK;
}
