/* SPDX-License-Identifier: Apache-2.0
 * Native-Dhara volume integration; existing ROS/RSM/HPKE protocol retained. */
#include "recording_volume.h"
#include "recording_aead_selftest.h"
#include <psa/crypto.h>
#include <string.h>
#define RV_MAGIC UINT32_C(0x52564f31)
static void wipe(void *p,size_t n){volatile uint8_t *b=p;while(n--)*b++=0;}
static int overlap(const void *a,size_t an,const void *b,size_t bn)
{uintptr_t x=(uintptr_t)a,y=(uintptr_t)b;return an>UINTPTR_MAX-x||bn>UINTPTR_MAX-y||(x<y+bn&&y<x+an);}
static int ext(const struct recording_volume *v,const void *p,size_t n)
{return p&&!overlap(v,sizeof(*v),p,n);}
static int fence(struct recording_volume *v){v->fault=1;v->state=RV_FENCED;return RV_FAULT;}
static int lock(struct recording_volume *v)
{if(!v||v->magic!=RV_MAGIC)return RV_ARGUMENT;unsigned x=0;return atomic_compare_exchange_strong(&v->gate,&x,1)?0:RV_BUSY;}
static uint64_t now(void *u){struct recording_volume *v=u;v->observed_ms=v->target.now_ms(v->target.user);return v->observed_ms;}
static int check(struct recording_volume *v,int writing,uint64_t d)
{
 uint64_t n=now(v);
 if(v->fault||n<v->last_now||n>=d||v->native.fault||v->metadata.fault||v->store.status.fault||
    recording_hpke_psa_faulted(&v->crypto))return fence(v);
 v->last_now=n;
 if(v->hooks.check(v->hooks.user,writing,d))return fence(v);
 n=now(v);if(n<v->last_now||n>=d)return fence(v);v->last_now=n;return 0;
}
static int begin(struct recording_volume *v,uint64_t d)
{int rc=lock(v);if(rc)return rc;if(!v->target.now_ms){atomic_store(&v->gate,0);return RV_REFUSED;}
 uint64_t n=now(v);v->deadline=d>n&&d-n>120000U?n+120000U:d;
 if(check(v,0,v->deadline)){atomic_store(&v->gate,0);return RV_FAULT;}return 0;}
static int done(struct recording_volume *v,int rc)
{if(!v->fault&&check(v,0,v->deadline))rc=RV_FAULT;atomic_store(&v->gate,0);return v->fault?RV_FAULT:rc;}
static int hashing(void *u,const uint8_t *p,size_t n,uint8_t *out,size_t cap,size_t *actual)
{(void)u;return psa_hash_compute(PSA_ALG_SHA_256,p,n,out,cap,actual)==PSA_SUCCESS?0:-1;}
static int same_spec(const struct owned_volume_spec *a,const struct owned_volume_spec *b)
{return a->generation==b->generation&&!memcmp(a->device_id,b->device_id,16)&&!memcmp(a->volume_id,b->volume_id,16)&&
 !memcmp(a->recipient_fingerprint,b->recipient_fingerprint,32)&&!memcmp(a->preservation_manifest_sha256,b->preservation_manifest_sha256,32)&&
 !memcmp(a->map_blocks,b->map_blocks,sizeof(a->map_blocks))&&!memcmp(a->control_blocks,b->control_blocks,sizeof(a->control_blocks));}
static int configuration(struct recording_volume *v,uint32_t expected)
{
 struct recording_configuration c;
 if(rcfg_get(&c)||c.phase!=expected||c.fault_banks||!same_spec(&c.spec,&v->configuration.spec)||
    memcmp(c.descriptor,v->configuration.descriptor,512)||memcmp(c.recipient,v->configuration.recipient,65))return fence(v);
 v->configuration=c;return 0;
}
static int root_ok(struct recording_volume *v)
{int full=rcfg_is_full(&v->configuration);
 return v->root_valid&&v->ros_valid&&v->root.capacity==(full?RLL_CAPACITY:RV_CAPACITY)&&
 v->root.ros_capacity==(full?RLL_ROS_CAPACITY:RV_ROS_CAPACITY)&&v->root.slots==(full?RLL_SLOTS:22U)&&
 !memcmp(v->root.descriptor_digest,rcfg_descriptor_digest(&v->configuration),32)&&!memcmp(v->root.root_digest,v->metadata.root_digest,32);}
static int yield_job(void *u,uint64_t d)
{struct recording_volume *v=u;return check(v,0,d)||v->hooks.yield(v->hooks.user,d)||check(v,0,d)?-1:0;}
static int acquire(void *u,uint64_t d)
{struct recording_volume *v=u;if(v->leased||check(v,0,d))return -1;
 if(v->hooks.acquire(v->hooks.user,d))return fence(v);
 v->leased=1;return check(v,0,d);}
static int release(void *u,uint64_t d)
{struct recording_volume *v=u;if(!v->leased||check(v,0,d))return -1;
 if(v->hooks.release(v->hooks.user,d))return fence(v);
 v->leased=0;return check(v,0,d);}
static int target_authorize(void *u,uint32_t access,uint32_t row,uint64_t d)
{struct recording_volume *v=u;(void)row;return !v->leased||check(v,access!=NOP_READ,d)?-1:0;}
static int app_admit(void *u,int writing,uint64_t d)
{struct recording_volume *v=u;if(check(v,writing,d)||!v->leased)return -1;
 if(writing&&v->state!=RV_WRITABLE&&v->state!=RV_PROVISIONING)return -1;
 return v->state==RV_PROVISIONING||v->state==RV_MOUNTING||v->state==RV_READ_ONLY||v->state==RV_WRITABLE?0:-1;}
static int read_map(void *u,uint32_t s,uint8_t *p,uint64_t d)
{struct recording_volume *v=u;return app_admit(v,0,d)?-1:rns_read(&v->native,s,p,d);}
static int write_map(void *u,uint32_t s,const uint8_t *p,uint64_t d)
{struct recording_volume *v=u;return app_admit(v,1,d)?-1:rns_write(&v->native,s,p,d);}
static int sync_map(void *u,uint64_t d)
{struct recording_volume *v=u;return app_admit(v,1,d)?-1:rns_sync(&v->native,d);}
static int create(void *u,const struct rsm_volume *volume,uint64_t d)
{struct recording_volume *v=u;return app_admit(v,1,d)||!v->confirmed||v->state!=RV_PROVISIONING||
 memcmp(volume->descriptor_digest,rcfg_descriptor_digest(&v->configuration),32)?-1:0;}
static int receipt(void *u,const struct rsm_volume *volume,const struct rsm_receipt *r,uint64_t d)
{struct recording_volume *v=u;return app_admit(v,1,d)||v->hooks.approve_receipt(v->hooks.user,volume,r,d)||check(v,1,d)?-1:0;}
static int deletion(void *u,const struct rsm_volume *volume,const uint8_t *op,const struct rsm_terminal *t,uint64_t d)
{struct recording_volume *v=u;return app_admit(v,1,d)||v->hooks.approve_delete(v->hooks.user,volume,op,t,d)||check(v,1,d)?-1:0;}
static int cancelled(void *u){struct recording_volume *v=u;return v->fault!=0;}
static int seal(void *u,const uint8_t *recipient,const uint8_t *info,const uint8_t *plain,size_t n,
 uint8_t *enc,uint8_t *cipher,size_t cap,size_t *actual)
{struct recording_volume *v=u;
 if(v->state!=RV_WRITABLE||memcmp(recipient,v->configuration.recipient,65))return -1;
 if(plain==cipher)return recording_hpke_psa_seal_in_place(&v->crypto,recipient,65,info,161,NULL,0,cipher,n,cap,enc,65,actual);
 return recording_hpke_psa_seal(&v->crypto,recipient,65,info,161,NULL,0,plain,n,enc,65,cipher,cap,actual);}
struct retired_submission {struct recording_volume *volume;uint64_t deadline;int result;};
static int submit_retired(void *user,const struct rsm_volume *proof,const uint8_t *ids,uint32_t count)
{
 struct retired_submission *s=user;struct recording_volume *v=s->volume;
 if(proof->capacity!=RLL_CAPACITY||proof->generation!=v->binding.generation||
  memcmp(proof->device,v->binding.device_id,16)||memcmp(proof->volume,v->binding.volume_id,16)||
  memcmp(proof->recipient,v->binding.key_fingerprint,32)||memcmp(proof->descriptor_digest,rcfg_descriptor_digest(&v->configuration),32))return -1;
 s->result=ros_recover_retired(&v->store,&v->binding,ids,count,s->deadline);
 return s->result==ROS_OK||s->result==ROS_PENDING?0:-1;
}
static int recover_recordings(struct recording_volume *v)
{
 _Static_assert(RSM_TOMBSTONES==ROS_RETIRED_MAX,"Retained deletion bounds");
 uint8_t retired_ids[RSM_TOMBSTONES][16];uint32_t count=0;
 uint64_t start=now(v),store_deadline=v->deadline;
 int full=rcfg_is_full(&v->configuration);uint64_t limit=full?ROS_EXTENT_RECOVER_MS:ROS_JOB_MAX_MS;
 if(store_deadline>start&&store_deadline-start>limit)store_deadline=start+limit;
 int rc;
 if(full){struct retired_submission s={v,store_deadline,ROS_FAILED};
  if(rsm_visit_extent_deleted(&v->metadata,&s,submit_retired))return fence(v);rc=s.result;
 }else{
  if(rsm_deleted_snapshot(&v->metadata,retired_ids,&count))return fence(v);
  rc=ros_recover_retired(&v->store,&v->binding,&retired_ids[0][0],count,store_deadline);
 }
 wipe(retired_ids,sizeof(retired_ids));if(rc==ROS_OK)return 0;
 if(rc!=ROS_PENDING)return fence(v);
 for(unsigned steps=0;steps<(full?RLL_SLOTS*4U+RLL_ROOTS*4U:1024U)&&rc==ROS_PENDING;++steps){if(check(v,0,store_deadline))return RV_FAULT;rc=ros_step(&v->store);if(yield_job(v,store_deadline))return fence(v);}
 if(rc!=ROS_READY||ros_finish(&v->store))return fence(v);
 return 0;
}
static int applications(struct recording_volume *v,int creating)
{
 _Static_assert(RNS_CAPACITY==RV_CAPACITY,"Application capacity binding");
 struct rsm_volume volume={0};memcpy(volume.device,v->binding.device_id,16);memcpy(volume.volume,v->binding.volume_id,16);
 int full=rcfg_is_full(&v->configuration);
 memcpy(volume.recipient,v->binding.key_fingerprint,32);memcpy(volume.descriptor_digest,rcfg_descriptor_digest(&v->configuration),32);
 volume.generation=v->binding.generation;volume.capacity=full?RLL_CAPACITY:RV_CAPACITY;
 struct rsm_port metadata={v,now,app_admit,read_map,write_map,sync_map,create,receipt,deletion};
 if((full?rsm_init_extent(&v->metadata,&volume,&v->hash,&metadata):rsm_init(&v->metadata,&volume,&v->hash,&metadata))||rsm_open(&v->metadata,(uint32_t)creating,v->deadline)||
    rsm_get_root_proof(&v->metadata,&v->root))return fence(v);
 v->root_valid=1;
 struct ros_port store={v,now,cancelled,app_admit,read_map,write_map,sync_map,seal};
 if((full?ros_init_full(&v->store,&store,&v->hash,&v->binding,v->configuration.recipient):
    ros_init(&v->store,&store,&v->hash,&v->binding,v->configuration.recipient,RV_ROS_CAPACITY))||
    recover_recordings(v))return fence(v);
 v->ros_valid=1;return root_ok(v)?0:fence(v);
}

static int native_admit(void *u,uint32_t access,uint32_t row,uint64_t d)
{
 struct recording_volume *v=u;
 return !v->leased||check(v,access!=NOP_READ,d)||
 (access!=NOP_READ&&(v->state!=RV_PROVISIONING&&v->state!=RV_WRITABLE))||
 v->hooks.usb_power(v->hooks.user,access,row,d)||check(v,access!=NOP_READ,d)?-1:0;
}
int rv_init(struct recording_volume *v,const struct rv_hooks *h)
{
 if(!v||v->magic||!ext(v,h,sizeof(*h))||!h->acquire||!h->release||!h->check||!h->usb_power||!h->yield||
 !h->confirm_provision||!h->approve_receipt||!h->approve_delete||!atomic_is_lock_free(&v->gate))return RV_ARGUMENT;
 v->hooks=*h;v->magic=RV_MAGIC;int rc=rcfg_get(&v->configuration);
 if(rc==1)return RV_EMPTY;
 if(rc||v->configuration.fault_banks||
  (v->configuration.phase!=RCFG_PROVISIONING&&v->configuration.phase!=RCFG_ACTIVE&&
   !(rcfg_is_full(&v->configuration)&&v->configuration.phase==RCFG_PREPARED)))return fence(v);
 v->hash=(struct owned_page_hash){NULL,hashing};struct owned_volume_decoded decoded;
 struct recording_extent_identity identity;rcfg_extent_identity(&v->configuration,&identity);
 int full=rcfg_is_full(&v->configuration);
 if((full?rex_validate_full(v->configuration.descriptor,&identity,&v->hash):
    owned_volume_descriptor_validate(v->configuration.descriptor,512,&v->configuration.spec,&v->hash,&decoded))||
    recording_hpke_psa_init(&v->crypto)||recording_aead_selftest(v->store.container,sizeof(v->store.container)))return fence(v);
 struct nop_nrf_owner owner={v,acquire,release,target_authorize};
 if(nand_owned_phy_nrf_bind(&v->target,&v->native.phy,&owner))return fence(v);
 const struct rns_hooks hooks={v,native_admit,yield_job};
 if(full?rns_init_full(&v->native,v->configuration.descriptor,&identity,&v->target,&v->hash,&hooks):
    rns_init(&v->native,&decoded,&v->target,&v->hash,&hooks))return fence(v);
 memcpy(v->binding.device_id,v->configuration.spec.device_id,16);
 memcpy(v->binding.volume_id,v->configuration.spec.volume_id,16);
 memcpy(v->binding.key_fingerprint,v->configuration.spec.recipient_fingerprint,32);
 v->binding.generation=v->configuration.spec.generation;
 static const uint8_t domain[]="OpenPendant native Dhara disposable format v1";
 uint8_t bytes[sizeof(domain)+512];size_t actual=0;
 memcpy(bytes,domain,sizeof(domain));memcpy(bytes+sizeof(domain),v->configuration.descriptor,512);
 rc=hashing(NULL,bytes,sizeof(bytes),v->confirmation,32,&actual);wipe(bytes,sizeof(bytes));
 if(rc||actual!=32)return fence(v);
 v->state=v->configuration.phase!=RCFG_ACTIVE?RV_PREPARED:RV_CONFIGURED;return 0;
}
/* Historical recovery authority and control journals do not describe this
 * format. Do not invoke, consume, or fabricate their old transactions. */
int rv_recovery_init(struct recording_volume *v,const struct rv_hooks *h,
 const struct rv_recovery_authorizer *a,const struct rv_recovery_proof *p)
{(void)v;(void)h;(void)a;(void)p;return RV_REFUSED;}
int rv_recovery_run(struct recording_volume *v,uint64_t d){(void)v;(void)d;return RV_REFUSED;}
int rv_confirmation(struct recording_volume *v,uint8_t out[32])
{
 if(!v||!ext(v,out,32))return RV_ARGUMENT;int rc=lock(v);if(rc)return rc;
 if(v->fault||v->state!=RV_PREPARED)rc=RV_REFUSED;
 else memcpy(out,v->confirmation,32);atomic_store(&v->gate,0);return rc;
}
int rv_provision(struct recording_volume *v,const uint8_t confirmation[32],uint64_t d)
{
 if(!v||!ext(v,confirmation,32))return RV_ARGUMENT;
 int rc=begin(v,d);if(rc)return rc;
 if(rcfg_is_full(&v->configuration))return done(v,RV_REFUSED); /* separate incremental full format API */
 if(v->state!=RV_PREPARED||v->attempted||memcmp(confirmation,v->confirmation,32))return done(v,RV_REFUSED);
 if(configuration(v,RCFG_PROVISIONING)||
    v->hooks.confirm_provision(v->hooks.user,&v->configuration,confirmation,v->deadline)||
    check(v,1,v->deadline))return done(v,fence(v));
 v->attempted=1;v->confirmed=1;v->state=RV_PROVISIONING;
 /* Explicitly overwrite the already disposable TEST pool, not its two old
  * control banks. Publish the app root durably before marking config ACTIVE.
  * A cut before ACTIVE is incomplete, never automatically reformatted. */
 if(rns_format(&v->native,v->deadline)||applications(v,1)||rns_sync(&v->native,v->deadline)||
    rcfg_advance(RCFG_PROVISIONING,RCFG_ACTIVE)||configuration(v,RCFG_ACTIVE))return done(v,fence(v));
 v->state=RV_WRITABLE;return done(v,0);
}
int rv_mount(struct recording_volume *v,uint64_t d)
{int rc=begin(v,d);if(rc)return rc;if(v->state!=RV_CONFIGURED||v->attempted)return done(v,RV_REFUSED);
 v->attempted=1;if(configuration(v,RCFG_ACTIVE))return done(v,RV_FAULT);v->state=RV_MOUNTING;
 if(rns_mount(&v->native,v->deadline)||applications(v,0))return done(v,fence(v));
 v->state=RV_READ_ONLY;return done(v,0);}
int rv_provision_full(struct recording_volume *v,const uint8_t confirmation[32],uint64_t d)
{
 if(!v||!ext(v,confirmation,32))return RV_ARGUMENT;
 int rc=lock(v);if(rc)return rc;
 if(!v->target.now_ms){atomic_store(&v->gate,0);return RV_REFUSED;}
 uint64_t n=now(v);
 if(d<=n||d-n>3600000U){atomic_store(&v->gate,0);return RV_ARGUMENT;}
 v->deadline=d;
 if(check(v,0,d))return done(v,RV_FAULT);
 if(!rcfg_is_full(&v->configuration)||v->state!=RV_PREPARED||v->attempted||v->configuration.phase!=RCFG_PREPARED||
  memcmp(confirmation,v->confirmation,32))return done(v,RV_REFUSED);
 if(configuration(v,RCFG_PREPARED)||v->hooks.confirm_provision(v->hooks.user,&v->configuration,confirmation,d)||check(v,1,d))return done(v,fence(v));
 v->attempted=1;v->confirmed=1;
 if(rcfg_advance(RCFG_PREPARED,RCFG_PROVISIONING)||configuration(v,RCFG_PROVISIONING))return done(v,fence(v));
 v->state=RV_PROVISIONING;
 rc=rns_format_begin(&v->native,d);
 for(uint32_t steps=0;rc==RNS_MORE&&steps<=RLL_BLOCKS;++steps){
  if(yield_job(v,d))return done(v,fence(v));
  rc=rns_format_step(&v->native,d);
 }
 if(rc||applications(v,1)||rns_sync(&v->native,d)||rcfg_advance(RCFG_PROVISIONING,RCFG_ACTIVE)||configuration(v,RCFG_ACTIVE))return done(v,fence(v));
 v->state=RV_WRITABLE;return done(v,0);
}
int rv_grant(struct recording_volume *v,uint64_t d)
{int rc=begin(v,d);if(rc)return rc;if(v->state!=RV_READ_ONLY||!root_ok(v))return done(v,RV_REFUSED);
 if(configuration(v,RCFG_ACTIVE)||rns_grant(&v->native,v->deadline))return done(v,fence(v));
 v->state=RV_WRITABLE;return done(v,0);}
int rv_complete_full(struct recording_volume *v,const uint8_t confirmation[32],uint64_t d)
{
 if(!v||!ext(v,confirmation,32))return RV_ARGUMENT;
 int rc=lock(v);if(rc)return rc;
 if(!v->target.now_ms){atomic_store(&v->gate,0);return RV_REFUSED;}
 uint64_t n=now(v);v->deadline=d>n&&d-n>300000U?n+300000U:d;
 if(check(v,0,v->deadline))return done(v,RV_FAULT);
 if(!rcfg_is_full(&v->configuration)||v->state!=RV_PREPARED||v->attempted||
  v->configuration.phase!=RCFG_PROVISIONING||memcmp(confirmation,v->confirmation,32))return done(v,RV_REFUSED);
 if(configuration(v,RCFG_PROVISIONING)||v->hooks.confirm_provision(v->hooks.user,&v->configuration,confirmation,v->deadline))return done(v,fence(v));
 v->attempted=1;v->state=RV_MOUNTING;
 /* No format call: resume and authenticate the durable native root first.
  * RSM creation requires root AND every companion page to be absent; partial
  * metadata is rejected without repair. Existing complete metadata is read.
  * Original identity/configuration/recipient remain unchanged. */
 if(rns_mount(&v->native,v->deadline)||rns_grant(&v->native,v->deadline))return done(v,fence(v));
 v->confirmed=1;v->state=RV_PROVISIONING;
 if(applications(v,1)||rns_sync(&v->native,v->deadline)||
  rcfg_advance(RCFG_PROVISIONING,RCFG_ACTIVE)||configuration(v,RCFG_ACTIVE))return done(v,fence(v));
 v->state=RV_WRITABLE;return done(v,0);
}
int rv_extend_metadata(struct recording_volume *v,uint64_t d)
{int rc=begin(v,d);if(rc)return rc;
 if(v->state!=RV_WRITABLE||!root_ok(v)||v->store.status.job!=ROS_NONE||atomic_load(&v->metadata.gate))return done(v,RV_REFUSED);
 if(configuration(v,RCFG_ACTIVE)||rsm_extend(&v->metadata,v->deadline)||
    rsm_get_root_proof(&v->metadata,&v->root)||!root_ok(v))return done(v,fence(v));
 return done(v,0);}
int rv_close(struct recording_volume *v,uint64_t d)
{int rc=begin(v,d);if(rc)return rc;
 if((v->state!=RV_READ_ONLY&&v->state!=RV_WRITABLE)||v->store.status.job!=ROS_NONE||atomic_load(&v->metadata.gate))return done(v,RV_REFUSED);
 if(rns_close(&v->native,v->deadline))return done(v,fence(v));
 v->state=RV_CLOSED;return done(v,0);}
int rv_suspend(struct recording_volume *v,uint64_t d)
{int rc=begin(v,d);if(rc)return rc;
 if((v->state!=RV_READ_ONLY&&v->state!=RV_WRITABLE)||v->store.status.job!=ROS_NONE||atomic_load(&v->metadata.gate))return done(v,RV_REFUSED);
 if(rns_suspend(&v->native,v->deadline))return done(v,fence(v));
 v->state=RV_SUSPENDED;return done(v,0);}
int rv_reopen(struct recording_volume *v,uint64_t d)
{int rc=begin(v,d);if(rc)return rc;
 if(v->state!=RV_SUSPENDED||!root_ok(v))return done(v,RV_REFUSED);
 if(configuration(v,RCFG_ACTIVE))return done(v,RV_FAULT);
 v->state=RV_MOUNTING;
 if(rns_reopen(&v->native,v->deadline)||recover_recordings(v))return done(v,fence(v));
 v->state=RV_READ_ONLY;return done(v,0);}
int rv_access(struct recording_volume *v,struct ros_context **store,struct recording_sync_metadata **metadata,struct es_binding *binding)
{
 if(!v||!ext(v,store,sizeof(*store))||!ext(v,metadata,sizeof(*metadata))||!ext(v,binding,sizeof(*binding))||
 overlap(store,sizeof(*store),metadata,sizeof(*metadata))||overlap(store,sizeof(*store),binding,sizeof(*binding))||
 overlap(metadata,sizeof(*metadata),binding,sizeof(*binding)))return RV_ARGUMENT;
 int rc=lock(v);if(rc)return rc;
 if(v->fault||v->native.fault||v->metadata.fault||v->store.status.fault||!root_ok(v)||
 (v->state!=RV_READ_ONLY&&v->state!=RV_WRITABLE))rc=RV_REFUSED;
 else{*store=&v->store;*metadata=&v->metadata;*binding=v->binding;}
 atomic_store(&v->gate,0);return rc;
}
