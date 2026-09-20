/* SPDX-License-Identifier: Apache-2.0
 * Fixed full-chip profile only. Included after the common checked I/O helpers.
 * Each page is one atomic logical-map publication. No multi-page mutable root,
 * giant receipt cache, implicit delete, tombstone pruning or automatic repair. */
static uint32_t rsmx_sector(unsigned kind,unsigned index)
{return kind==1?RLL_METADATA_ROOT:kind==2?RLL_RECEIPT_BASE+index:RLL_TOMBSTONE_BASE+index;}
static unsigned rsmx_limit(unsigned kind,unsigned index)
{if(kind==2)return 32;unsigned left=RLL_TOMBSTONES-index*14U;return left>14U?14U:left;}
static int rsmx_receipt_valid(const struct rsm_receipt *r)
{return id(r->recording,16)&&r->sequence<RLL_SLOTS&&r->container_bytes>=425&&r->container_bytes<=34357&&id(r->digest,32);}
static int rsmx_terminal_valid(const struct rsm_terminal *t)
{return id(t->recording,16)&&id(t->manifest,32)&&t->segments<=RLL_SLOTS&&
 (t->state==1||t->state==2)&&t->revision==(uint64_t)t->segments*4U+t->state;}
static int rsmx_deleted(const struct recording_sync_metadata *c,const uint8_t record[16])
{for(uint32_t i=0;i<c->deleted_count;++i)if(!memcmp(c->deleted_ids[i],record,16))return RSM_OK;return RSM_NOT_FOUND;}
static void rsmx_header(struct recording_sync_metadata *c,uint8_t *p,unsigned kind,unsigned index,uint64_t rev,unsigned count)
{
 memset(p,255,2048);memset(p,0,192);memcpy(p,"OPNDSM3",7);put32(p+8,kind);put32(p+12,index);
 memcpy(p+16,c->volume.device,16);memcpy(p+32,c->volume.volume,16);put64(p+48,c->volume.generation);
 memcpy(p+56,c->volume.recipient,32);memcpy(p+88,c->volume.descriptor_digest,32);
 put32(p+120,RLL_CAPACITY);put32(p+124,RLL_SLOTS);put32(p+128,count);put64(p+136,rev);
 if(kind!=1)memcpy(p+144,c->root_digest,32);
 put32(p+176,rsmx_sector(kind,index));put32(p+180,RLL_ROOTS);put32(p+184,RLL_TOMBSTONES);put32(p+188,3);
}
static int rsmx_validate(struct recording_sync_metadata *c,const uint8_t *p,unsigned kind,unsigned index)
{
 uint8_t sum[32];unsigned count=get32(p+128);uint64_t rev=get64(p+136);
 if(kind<1||kind>3||(kind==1&&index)||(kind==2&&index>=RLL_RECEIPT_PAGES)||(kind==3&&index>=RLL_TOMBSTONE_PAGES)||
 memcmp(p,"OPNDSM3\0",8)||get32(p+8)!=kind||get32(p+12)!=index||memcmp(p+16,c->volume.device,16)||
 memcmp(p+32,c->volume.volume,16)||get64(p+48)!=c->volume.generation||memcmp(p+56,c->volume.recipient,32)||
 memcmp(p+88,c->volume.descriptor_digest,32)||get32(p+120)!=RLL_CAPACITY||get32(p+124)!=RLL_SLOTS||
 get32(p+132)||!rev||rev>INT64_MAX||get32(p+176)!=rsmx_sector(kind,index)||get32(p+180)!=RLL_ROOTS||
 get32(p+184)!=RLL_TOMBSTONES||get32(p+188)!=3||hash(c,p,sum)||memcmp(sum,p+2016,32))return fault(c);
 if(kind==1)return count||rev!=1||!all(p+144,32,0)||!all(p+192,1824,255)||memcmp(p+2016,c->root_digest,32)?fault(c):0;
 if(memcmp(p+144,c->root_digest,32)||count>rsmx_limit(kind,index)||!all(p+1984,32,255))return fault(c);
 if(kind==2){
  unsigned seen=0;
  for(unsigned i=0;i<32;++i){const uint8_t *e=p+192+i*56;struct rsm_receipt r;
   if(all(e,56,255))continue;receipt_get(e,&r);if(!rsmx_receipt_valid(&r))return fault(c);++seen;
  }
  return seen!=count||rev<(uint64_t)count+1U||(!count&&rev!=1)?fault(c):0;
 }
 uint64_t previous=1;
 for(unsigned i=0;i<count;++i){const uint8_t *e=p+192+i*128;struct rsm_tombstone t;tomb_get(e,&t);
  if(!id(t.operation,16)||!rsmx_terminal_valid(&t.terminal)||t.revision<=t.terminal.revision||t.revision<=previous||
     t.revision>INT64_MAX||!all(e+88,40,0))return fault(c);
  for(unsigned j=0;j<i;++j){const uint8_t *old=p+192+j*128;
   if(!memcmp(old,e,16)||!memcmp(old+32,e+32,32))return fault(c);}
  previous=t.revision;
 }
 return previous!=rev||!all(p+192+count*128,1792-count*128,255)?fault(c):0;
}
static int rsmx_read(struct recording_sync_metadata *c,unsigned kind,unsigned index,uint8_t *p)
{
 if(admit(c,0))return RSM_FAULT;
 int rc=c->port.read(c->port.user,rsmx_sector(kind,index),p,c->deadline);
 if(check(c)||rc<0||rc>1)return fault(c);
 return rc;
}
static int rsmx_publish(struct recording_sync_metadata *c,unsigned kind,unsigned index)
{
 if(hash(c,c->work,c->work+2016)||rsmx_validate(c,c->work,kind,index)||admit(c,1)||
 c->port.write(c->port.user,rsmx_sector(kind,index),c->work,c->deadline)||check(c)||admit(c,1)||
 c->port.sync(c->port.user,c->deadline)||check(c)||rsmx_read(c,kind,index,c->verify)||
 rsmx_validate(c,c->verify,kind,index)||memcmp(c->work,c->verify,2048))return fault(c);
 return 0;
}
int rsm_init_extent(struct recording_sync_metadata *c,const struct rsm_volume *v,const struct owned_page_hash *h,const struct rsm_port *p)
{
 if(!c||c->magic||!ext(c,v,sizeof(*v))||!ext(c,h,sizeof(*h))||!ext(c,p,sizeof(*p))||!h->sha256||
 !p->now_ms||!p->admit||!p->read||!p->write||!p->sync||!p->approve_create||!p->approve_receipt||!p->approve_delete||
 !id(v->device,16)||!id(v->volume,16)||!id(v->recipient,32)||!id(v->descriptor_digest,32)||
 !v->generation||v->generation>INT64_MAX||v->capacity!=RLL_CAPACITY||!atomic_is_lock_free(&c->gate))return RSM_ARGUMENT;
 c->volume=*v;c->hash=*h;c->port=*p;c->format=3;c->extent_profile=1;
 rsmx_header(c,c->work,1,0,1,0);if(hash(c,c->work,c->root_digest))return RSM_FAULT;
 wipe(c->work,2048);c->magic=MAGIC;return 0;
}
static int rsmx_open(struct recording_sync_metadata *c,uint32_t create)
{
 if(c->attempted)return RSM_REFUSED;c->attempted=1;c->delete_revision=1;
 int missing=rsmx_read(c,1,0,c->work);if(missing<0)return RSM_FAULT;
 if(!missing&&rsmx_validate(c,c->work,1,0))return RSM_FAULT;
 /* An absent root admits creation only if EVERY companion is absent too.
  * A cut during provisioning remains incomplete; it is never auto-retried. */
 for(unsigned kind=2;kind<=3;++kind){unsigned pages=kind==2?RLL_RECEIPT_PAGES:RLL_TOMBSTONE_PAGES;
  for(unsigned index=0;index<pages;++index){
   int rc=rsmx_read(c,kind,index,c->work);if(rc<0||rc!=missing)return fault(c);
   if(missing)continue;
   if(rsmx_validate(c,c->work,kind,index))return RSM_FAULT;
   if(kind==3){unsigned n=get32(c->work+128);
    if(n&&c->deleted_count!=index*14U)return fault(c);
    /* Cross-page operation/hash uniqueness without another in-RAM index.
     * Bounded to 37 pages; no cached UUID revocation memory is overwritten. */
    for(unsigned prior=0;n&&prior<index;++prior){
     if(rsmx_read(c,3,prior,c->verify)||rsmx_validate(c,c->verify,3,prior))return fault(c);
     unsigned old_count=get32(c->verify+128);
     for(unsigned i=0;i<n;++i)for(unsigned j=0;j<old_count;++j){
      const uint8_t *a=c->work+192+i*128,*b=c->verify+192+j*128;
      if(!memcmp(a,b,16)||!memcmp(a+32,b+32,32))return fault(c);
     }
    }
    for(unsigned i=0;i<n;++i){struct rsm_tombstone t;tomb_get(c->work+192+i*128,&t);
     if(c->deleted_count>=RLL_TOMBSTONES||t.revision<=c->delete_revision||rsmx_deleted(c,t.terminal.recording)==RSM_OK)return fault(c);
     memcpy(c->deleted_ids[c->deleted_count++],t.terminal.recording,16);c->delete_revision=t.revision;
    }
   }
  }
 }
 if(missing){
  if(!create)return RSM_NOT_CREATED;
  if(admit(c,1)||c->port.approve_create(c->port.user,&c->volume,c->deadline)||check(c))return fault(c);
  for(unsigned kind=2;kind<=3;++kind){unsigned pages=kind==2?RLL_RECEIPT_PAGES:RLL_TOMBSTONE_PAGES;
   for(unsigned index=0;index<pages;++index){rsmx_header(c,c->work,kind,index,1,0);if(rsmx_publish(c,kind,index))return RSM_FAULT;}
  }
  rsmx_header(c,c->work,1,0,1,0);if(rsmx_publish(c,1,0))return RSM_FAULT;
 }
 c->mounted=1;return 0;
}
static int rsmx_receipt(struct recording_sync_metadata *c,uint32_t slot,const struct rsm_receipt *r,int write)
{
 if(!c->mounted||!c->extent_profile)return RSM_REFUSED;
 if(rsmx_deleted(c,r->recording)==RSM_OK)return RSM_CONFLICT;
 unsigned index=slot/32U,at=slot%32U;
 if(rsmx_read(c,2,index,c->work)||rsmx_validate(c,c->work,2,index))return fault(c);
 uint8_t *entry=c->work+192+at*56;int empty=all(entry,56,255);
 if(!empty){struct rsm_receipt old;receipt_get(entry,&old);
  if(!memcmp(&old,r,sizeof(old)))return 0;
  if(!write)return RSM_NOT_FOUND;
  /* An old exact slot receipt can be replaced only when its recording was
   * durably deleted. Never evict a live receipt to make room. */
  if(rsmx_deleted(c,old.recording)!=RSM_OK)return RSM_CONFLICT;
 }else if(!write)return RSM_NOT_FOUND;
 uint64_t rev=get64(c->work+136);if(rev==INT64_MAX)return fault(c);
 if(admit(c,1)||c->port.approve_receipt(c->port.user,&c->volume,r,c->deadline)||check(c))return fault(c);
 receipt_put(entry,r);put64(c->work+136,rev+1);if(empty)put32(c->work+128,get32(c->work+128)+1);
 return rsmx_publish(c,2,index);
}
int rsm_receive_slot(struct recording_sync_metadata *c,uint32_t slot,const struct rsm_receipt *r,uint64_t d)
{
 if(!c||slot>=RLL_SLOTS||!ext(c,r,sizeof(*r))||!rsmx_receipt_valid(r))return RSM_ARGUMENT;
 int rc=begin(c,d);if(rc)return rc;return done(c,rsmx_receipt(c,slot,r,1));
}
static int rsmx_batch_page(struct recording_sync_metadata *c,const uint32_t *slots,
 const struct rsm_receipt *receipts,uint32_t count,unsigned page,int publish)
{
 if(rsmx_read(c,2,page,c->work)||rsmx_validate(c,c->work,2,page))return fault(c);
 uint64_t rev=get64(c->work+136);unsigned added=0,changed=0;
 for(uint32_t i=0;i<count;++i){
  if(slots[i]/32U!=page)continue;
  const struct rsm_receipt *r=receipts+i;
  if(rsmx_deleted(c,r->recording)==RSM_OK)return RSM_CONFLICT;
  uint8_t *entry=c->work+192+(slots[i]%32U)*56;int empty=all(entry,56,255);
  if(!empty){struct rsm_receipt old;receipt_get(entry,&old);
   if(!memcmp(&old,r,sizeof(old)))continue;
   if(rsmx_deleted(c,old.recording)!=RSM_OK)return RSM_CONFLICT;
  }
  if(admit(c,1)||c->port.approve_receipt(c->port.user,&c->volume,r,c->deadline)||check(c))return fault(c);
  ++changed;if(empty)++added;if(publish)receipt_put(entry,r);
 }
 if(rev>(uint64_t)INT64_MAX-changed)return fault(c);
 if(!publish||!changed)return 0;
 put64(c->work+136,rev+changed);put32(c->work+128,get32(c->work+128)+added);
 return rsmx_publish(c,2,page);
}
int rsm_receive_slots(struct recording_sync_metadata *c,const uint32_t *slots,
 const struct rsm_receipt *receipts,uint32_t count,uint64_t d)
{
 if(!c||!count||count>RSM_RECEIPT_BATCH_MAX||!ext(c,slots,count*sizeof(*slots))||
 !ext(c,receipts,count*sizeof(*receipts))||overlap(slots,count*sizeof(*slots),receipts,count*sizeof(*receipts)))return RSM_ARGUMENT;
 for(uint32_t i=0;i<count;++i){
  if(slots[i]>=RLL_SLOTS||!rsmx_receipt_valid(receipts+i))return RSM_ARGUMENT;
  for(uint32_t j=0;j<i;++j)if(slots[i]==slots[j])return RSM_ARGUMENT;
 }
 int rc=begin(c,d);if(rc)return rc;
 if(!c->mounted||!c->extent_profile)return done(c,RSM_REFUSED);
 /* Full preflight before ANY durable page changes. The exclusive owner and
  * immutable input prevent a later page conflict being hidden by earlier writes. */
 for(unsigned pass=0;pass<2;++pass)for(uint32_t i=0;i<count;++i){
  unsigned page=slots[i]/32U;int seen=0;
  for(uint32_t j=0;j<i;++j)if(slots[j]/32U==page)seen=1;
  if(seen)continue;
  rc=rsmx_batch_page(c,slots,receipts,count,page,(int)pass);
  if(rc)return done(c,rc);
 }
 return done(c,0);
}
int rsm_has_receipt_slot(struct recording_sync_metadata *c,uint32_t slot,const struct rsm_receipt *r,uint64_t d)
{
 if(!c||slot>=RLL_SLOTS||!ext(c,r,sizeof(*r))||!rsmx_receipt_valid(r))return RSM_ARGUMENT;
 int rc=begin(c,d);if(rc)return rc;return done(c,rsmx_receipt(c,slot,r,0));
}
static int rsmx_replay(struct recording_sync_metadata *c,const uint8_t op[16],const uint8_t digest[32],struct rsm_tombstone *out)
{
 unsigned pages=(c->deleted_count+13U)/14U;int found=0;
 for(unsigned index=0;index<pages;++index){
  if(rsmx_read(c,3,index,c->verify)||rsmx_validate(c,c->verify,3,index))return fault(c);
  unsigned n=get32(c->verify+128);
  for(unsigned i=0;i<n;++i){struct rsm_tombstone t;tomb_get(c->verify+192+i*128,&t);
   if(!memcmp(t.operation,op,16)){
    if(found||memcmp(t.terminal.manifest,digest,32))return RSM_CONFLICT;
    *out=t;found=1;
   }else if(!memcmp(t.terminal.manifest,digest,32))return RSM_CONFLICT;
  }
 }
 return found?RSM_OK:RSM_NOT_FOUND;
}
int rsm_replay_delete_paged(struct recording_sync_metadata *c,const uint8_t op[16],const uint8_t digest[32],struct rsm_tombstone *out,uint64_t d)
{
 if(!c||!ext(c,op,16)||!ext(c,digest,32)||!ext(c,out,sizeof(*out))||overlap(out,sizeof(*out),op,16)||
 overlap(out,sizeof(*out),digest,32)||!id(op,16)||!id(digest,32))return RSM_ARGUMENT;
 int rc=begin(c,d);if(rc)return rc;
 if(!c->mounted||!c->extent_profile)return done(c,RSM_REFUSED);
 struct rsm_tombstone result;rc=rsmx_replay(c,op,digest,&result);
 rc=done(c,rc);if(!rc)*out=result;return rc;
}
static int rsmx_delete(struct recording_sync_metadata *c,const uint8_t op[16],const struct rsm_terminal *t,uint64_t *revision)
{
 if(!c->mounted)return RSM_REFUSED;
 struct rsm_tombstone old;int rc=rsmx_replay(c,op,t->manifest,&old);
 if(rc==RSM_OK){if(memcmp(&old.terminal,t,sizeof(*t)))return RSM_CONFLICT;*revision=old.revision;return 0;}
 if(rc!=RSM_NOT_FOUND)return rc;
 if(rsmx_deleted(c,t->recording)==RSM_OK)return RSM_CONFLICT;
 if(c->deleted_count==RLL_TOMBSTONES)return RSM_FULL;
 uint64_t rev=c->delete_revision;if(rev==INT64_MAX)return fault(c);++rev;if(rev<=t->revision)rev=t->revision+1;
 unsigned index=c->deleted_count/14U,at=c->deleted_count%14U;
 if(rsmx_read(c,3,index,c->work)||rsmx_validate(c,c->work,3,index)||get32(c->work+128)!=at)return fault(c);
 if(admit(c,1)||c->port.approve_delete(c->port.user,&c->volume,op,t,c->deadline)||check(c))return fault(c);
 tomb_put(c->work+192+at*128,op,t,rev);put32(c->work+128,at+1);put64(c->work+136,rev);
 if(rsmx_publish(c,3,index))return RSM_FAULT;
 memcpy(c->deleted_ids[c->deleted_count++],t->recording,16);c->delete_revision=rev;*revision=rev;
 return 0;
}
