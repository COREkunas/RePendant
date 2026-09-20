/* SPDX-License-Identifier: Apache-2.0 */
#include "recording_volume.h"
#include <psa/crypto.h>
#include <string.h>
#define RV_MAGIC UINT32_C(0x52564f31)
static void wipe(void *p,size_t n){volatile uint8_t *b=p;while(n--)*b++=0;}
static int overlap(const void *a,size_t an,const void *b,size_t bn)
{uintptr_t x=(uintptr_t)a,y=(uintptr_t)b;return an>UINTPTR_MAX-x||bn>UINTPTR_MAX-y||(x<y+bn&&y<x+an);}
static int ext(const struct recording_volume *v,const void *p,size_t n)
{return p&&!overlap(v,sizeof(*v),p,n);}
static int fence(struct recording_volume *v)
{
 if(v->recovery_active&&!v->recovery_first_fault.valid){struct rv_recovery_first_fault *f=&v->recovery_first_fault;
  f->valid=1;f->stage=v->recovery_stage;f->rc=v->recovery_rc?v->recovery_rc:RV_FAULT;
  f->observed_ms=v->observed_ms;f->start_ms=v->recovery_start;f->parent_deadline=v->recovery_parent;
  f->preimage_deadline=v->recovery_preimage;f->mutation_deadline=v->recovery_mutation;
  f->phase=v->configuration.phase;f->revision=v->configuration.revision;f->leased=v->leased;
  f->stopped=v->storage.phy.stopped;f->ready=v->storage.phy.ready_known;f->starts=v->storage.phy.starts;
  f->a0=v->storage.phy.a0;f->b0=v->storage.phy.b0;f->c0=v->storage.phy.status;f->media_flags=v->storage.phy.media_flags;
  f->provider_fault=v->storage.fault;f->journal_fault=v->storage.journal.fault;f->map_fault=v->storage.map.fault;f->phy_fault=v->storage.phy.fault;
  f->epoch=v->storage.journal.current.epoch;f->high_water=v->storage.journal.current.high_water;f->provider=v->storage.first_fault;
 }
 v->fault=1;v->state=RV_FENCED;owned_dhara_revoke(&v->storage.map);return RV_FAULT;}
static int lock(struct recording_volume *v)
{if(!v||v->magic!=RV_MAGIC)return RV_ARGUMENT;unsigned x=0;return atomic_compare_exchange_strong(&v->gate,&x,1)?0:RV_BUSY;}
static uint64_t now(void *u){struct recording_volume *v=u;v->observed_ms=v->target.now_ms(v->target.user);return v->observed_ms;}
static int check(struct recording_volume *v,int writing,uint64_t d)
{
 uint64_t n=now(v);
 if(v->fault||n<v->last_now||n>=d||(v->recovery_active&&(d>v->deadline||n>=v->recovery_parent||(writing&&v->recovery_phase!=2)))||v->storage.fault||v->metadata.fault||v->store.status.fault||
    recording_hpke_psa_faulted(&v->crypto))return fence(v);
 v->last_now=n;
 int rc=v->hooks.check(v->hooks.user,writing,d);if(rc){v->recovery_rc=rc;return fence(v);}
 n=now(v);if(n<v->last_now||n>=d||(v->recovery_active&&n>=v->recovery_parent))return fence(v);v->last_now=n;return 0;
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
{return v->root_valid&&v->ros_valid&&v->root.capacity==RV_CAPACITY&&v->root.ros_capacity==RV_ROS_CAPACITY&&v->root.slots==22&&
 !memcmp(v->root.descriptor_digest,v->configuration.descriptor+352,32)&&!memcmp(v->root.root_digest,v->metadata.root_digest,32);}
static int policy(void *u,uint32_t reason,const struct owned_volume_decoded *volume,uint64_t epoch,uint32_t row,uint32_t flags,uint64_t d)
{
 struct recording_volume *v=u;
 if(check(v,reason!=OCJ_ADMIT_BOOT,d)||flags||!same_spec(&volume->spec,&v->configuration.spec)||
    memcmp(volume->digest,v->configuration.descriptor+352,32)||rcfg_faulted(volume->digest))return -1;
 if(reason==OCJ_ADMIT_BOOT)return v->state==RV_PROVISIONING||v->state==RV_MOUNTING||v->state==RV_READ_ONLY||v->state==RV_WRITABLE?0:-1;
 if(reason==OCJ_ADMIT_PROVISION)return v->state==RV_PROVISIONING&&v->confirmed&&v->configuration.phase==RCFG_PROVISIONING?0:-1;
 if(reason==OCJ_ADMIT_ACTIVE)return root_ok(v)&&(v->state==RV_PROVISIONING||v->state==RV_READ_ONLY||v->state==RV_WRITABLE)?0:-1;
 if(reason==OCJ_ADMIT_MAP)return v->state==RV_PROVISIONING||v->state==RV_WRITABLE||v->state==RV_READ_ONLY?0:-1;
 if(reason==OCJ_ADMIT_RECYCLE){
  uint32_t bank=v->storage.journal.current.bank;
  if(!epoch||bank>1||row==UINT32_MAX||row%64U||row/64U!=volume->spec.control_blocks[1U-bank])return -1;
  /* Explicit conditional target-only disposable-alternate re-erase policy.
   * It is not a general arbitrary-cut/bank-bitrot/endurance guarantee. */
  return v->hooks.usb_power(v->hooks.user,NOP_ERASE,row,d)||check(v,1,d)?-1:0;
 }
 return -1;
}
static int power(void *u,uint32_t access,uint32_t row,uint64_t d)
{struct recording_volume *v=u;return check(v,1,d)||v->hooks.usb_power(v->hooks.user,access,row,d)||check(v,1,d)?-1:0;}
static int latched(void *u,const uint8_t *digest,uint64_t d)
{struct recording_volume *v=u;return check(v,0,d)||memcmp(digest,v->configuration.descriptor+352,32)||rcfg_faulted(digest);}
static int latch(void *u,const uint8_t *digest,uint32_t bank,uint64_t d)
{struct recording_volume *v=u;int rc=rcfg_latch_fault(digest,bank);(void)check(v,1,d);fence(v);return rc;}
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
{struct recording_volume *v=u;return app_admit(v,0,d)?-1:rsp_read(&v->storage,s,p,d);}
static int write_map(void *u,uint32_t s,const uint8_t *p,uint64_t d)
{struct recording_volume *v=u;return app_admit(v,1,d)?-1:rsp_write(&v->storage,s,p,d);}
static int sync_map(void *u,uint64_t d)
{struct recording_volume *v=u;return app_admit(v,1,d)?-1:rsp_sync(&v->storage,d);}
static int create(void *u,const struct rsm_volume *volume,uint64_t d)
{struct recording_volume *v=u;return app_admit(v,1,d)||!v->confirmed||v->state!=RV_PROVISIONING||
 memcmp(volume->descriptor_digest,v->configuration.descriptor+352,32)?-1:0;}
static int receipt(void *u,const struct rsm_volume *volume,const struct rsm_receipt *r,uint64_t d)
{struct recording_volume *v=u;return app_admit(v,1,d)||v->hooks.approve_receipt(v->hooks.user,volume,r,d)||check(v,1,d)?-1:0;}
static int deletion(void *u,const struct rsm_volume *volume,const uint8_t *op,const struct rsm_terminal *t,uint64_t d)
{struct recording_volume *v=u;return app_admit(v,1,d)||v->hooks.approve_delete(v->hooks.user,volume,op,t,d)||check(v,1,d)?-1:0;}
static int cancelled(void *u){struct recording_volume *v=u;return v->fault!=0;}
static int seal(void *u,const uint8_t *recipient,const uint8_t *info,const uint8_t *plain,size_t n,
 uint8_t *enc,uint8_t *cipher,size_t cap,size_t *actual)
{struct recording_volume *v=u;
 if(v->state!=RV_WRITABLE||memcmp(recipient,v->configuration.recipient,65))return -1;
 return recording_hpke_psa_seal(&v->crypto,recipient,65,info,161,NULL,0,plain,n,enc,65,cipher,cap,actual);}
static int applications(struct recording_volume *v,int creating)
{
 uint32_t capacity=0;if(rsp_capacity(&v->storage,&capacity)||capacity<RV_CAPACITY)return fence(v);
 struct rsm_volume volume={0};memcpy(volume.device,v->binding.device_id,16);memcpy(volume.volume,v->binding.volume_id,16);
 memcpy(volume.recipient,v->binding.key_fingerprint,32);memcpy(volume.descriptor_digest,v->configuration.descriptor+352,32);
 volume.generation=v->binding.generation;volume.capacity=RV_CAPACITY;
 struct rsm_port metadata={v,now,app_admit,read_map,write_map,sync_map,create,receipt,deletion};
 if(rsm_init(&v->metadata,&volume,&v->hash,&metadata)||rsm_open(&v->metadata,(uint32_t)creating,v->deadline)||
    rsm_get_root_proof(&v->metadata,&v->root))return fence(v);
 v->root_valid=1;
 struct ros_port store={v,now,cancelled,app_admit,read_map,write_map,sync_map,seal};
 uint64_t start=now(v),store_deadline=v->deadline;
 if(store_deadline>start&&store_deadline-start>ROS_JOB_MAX_MS)store_deadline=start+ROS_JOB_MAX_MS;
 if(ros_init(&v->store,&store,&v->hash,&v->binding,v->configuration.recipient,RV_ROS_CAPACITY)||
    ros_recover(&v->store,store_deadline)!=ROS_PENDING)return fence(v);
 int rc=ROS_PENDING;
 for(unsigned steps=0;steps<1024&&rc==ROS_PENDING;++steps){if(check(v,0,store_deadline))return RV_FAULT;rc=ros_step(&v->store);if(yield_job(v,store_deadline))return fence(v);}
 if(rc!=ROS_READY||ros_finish(&v->store))return fence(v);
 v->ros_valid=1;return root_ok(v)?0:fence(v);
}
static int initialize(struct recording_volume *v,const struct rv_hooks *h,int recovery)
{
 if(!v||v->magic||!ext(v,h,sizeof(*h))||!h->acquire||!h->release||!h->check||!h->usb_power||!h->yield||
 !h->confirm_provision||!h->approve_receipt||!h->approve_delete||!atomic_is_lock_free(&v->gate))return RV_ARGUMENT;
 v->hooks=*h;v->magic=RV_MAGIC;int rc=rcfg_get(&v->configuration);
 if(rc==1)return RV_EMPTY;
 if(rc||v->configuration.fault_banks)return fence(v);
 if(v->configuration.phase==RCFG_PROVISIONING&&!recovery){v->state=RV_FENCED;v->fault=1;return RV_INCOMPLETE;}
 if(recovery?(v->configuration.phase!=RCFG_PROVISIONING||v->configuration.revision!=2):
    (v->configuration.phase!=RCFG_PREPARED&&v->configuration.phase!=RCFG_ACTIVE))return fence(v);
 for(unsigned i=0;i<32;i++)if(v->configuration.spec.map_blocks[i]!=1025U+i)return fence(v);
 if(v->configuration.spec.control_blocks[0]!=1057||v->configuration.spec.control_blocks[1]!=1058)return fence(v);
 v->hash=(struct owned_page_hash){NULL,hashing};
 if(recording_hpke_psa_init(&v->crypto))return fence(v);
 struct nop_nrf_owner owner={v,acquire,release,target_authorize};
 if(nand_owned_phy_nrf_bind(&v->target,&v->storage.phy,&owner))return fence(v);
 struct rsp_hal hal={v->target.user,v->target.now_ms,v->target.wait_us,v->target.open,v->target.transfer,v->target.close};
 struct rsp_policy p={v,policy,power,latched,latch,yield_job};
 if(rsp_init(&v->storage,v->configuration.descriptor,&v->configuration.spec,&v->hash,&hal,&p))return fence(v);
 memcpy(v->binding.device_id,v->configuration.spec.device_id,16);memcpy(v->binding.volume_id,v->configuration.spec.volume_id,16);
 memcpy(v->binding.key_fingerprint,v->configuration.spec.recipient_fingerprint,32);v->binding.generation=v->configuration.spec.generation;
 static const uint8_t domain[]="OpenPendant provision fixed volume v1";
 uint8_t bytes[sizeof(domain)+512+32];size_t actual=0;memcpy(bytes,domain,sizeof(domain));
 memcpy(bytes+sizeof(domain),v->configuration.descriptor,512);memcpy(bytes+sizeof(domain)+512,v->configuration.spec.preservation_manifest_sha256,32);
 rc=hashing(NULL,bytes,sizeof(bytes),v->confirmation,32,&actual);wipe(bytes,sizeof(bytes));
 if(rc||actual!=32)return fence(v);
 v->state=recovery?RV_RECOVERY_PREPARED:v->configuration.phase==RCFG_PREPARED?RV_PREPARED:RV_CONFIGURED;return 0;
}
int rv_init(struct recording_volume *v,const struct rv_hooks *h){return initialize(v,h,0);}
static int nonsentinel(const uint8_t *p,size_t n)
{unsigned z=0,f=0;while(n--){z|=*p;f|=*p++^255U;}return z&&f;}
static int recovery_config(struct recording_volume *v)
{
 static const uint8_t domain[]="OpenPendant phase2 recovery configuration v1";
 uint8_t bytes[sizeof(domain)+512+65+16],sum[32];size_t actual=0;
 if((v->recovery_authorized_valid&&memcmp(&v->recovery,&v->recovery_authorized,sizeof(v->recovery)))||
    configuration(v,RCFG_PROVISIONING)||v->configuration.revision!=2||v->configuration.spec.generation!=1)return -1;
 memcpy(bytes,domain,sizeof(domain));memcpy(bytes+sizeof(domain),v->configuration.descriptor,512);
 memcpy(bytes+sizeof(domain)+512,v->configuration.recipient,65);
 uint8_t *p=bytes+sizeof(domain)+577;
 for(unsigned i=0;i<8;i++)p[i]=(uint8_t)(v->configuration.revision>>(8*i));
 for(unsigned i=0;i<4;i++){p[8+i]=(uint8_t)(v->configuration.phase>>(8*i));p[12+i]=(uint8_t)(v->configuration.fault_banks>>(8*i));}
 int rc=hashing(NULL,bytes,sizeof(bytes),sum,32,&actual)||actual!=32||memcmp(sum,v->recovery.configuration_sha256,32);
 wipe(bytes,sizeof(bytes));wipe(sum,sizeof(sum));return rc;
}
int rv_recovery_init(struct recording_volume *v,const struct rv_hooks *h,
 const struct rv_recovery_authorizer *a,const struct rv_recovery_proof *p)
{
 if(!v||!ext(v,a,sizeof(*a))||!ext(v,p,sizeof(*p))||!a->consume||
    !nonsentinel(p->transaction,16)||!nonsentinel(p->configuration_sha256,32)||
    !nonsentinel(p->preimage_sha256,32)||!nonsentinel(p->control_digest,32)||!nonsentinel(p->capture_firmware_digest,32)||
    p->control_epoch!=5||p->lease_high_water!=130)return RV_ARGUMENT;
 int rc=initialize(v,h,1);if(rc)return rc;
 v->recovery=*p;v->recovery_authorizer=*a;
 return recovery_config(v)?fence(v):0;
}
static int recovery_admit(void *u,uint32_t phase,uint64_t d)
{
 struct recording_volume *v=u;
 if(!atomic_load(&v->gate)||!v->recovery_active||!v->confirmed||!v->attempted||v->state!=RV_PROVISIONING||
    phase!=v->recovery_phase||d!=v->deadline||
    (phase==1?(v->recovery_transitions||d!=v->recovery_preimage):
     (phase!=2||v->recovery_transitions!=1||!v->leased||d!=v->recovery_mutation)))return -1;
 return recovery_config(v)||check(v,0,d)?-1:0;
}
int rv_recovery_run(struct recording_volume *v,uint64_t d)
{
 int rc=lock(v);if(rc)return rc;
 if(v->state!=RV_RECOVERY_PREPARED||v->attempted){atomic_store(&v->gate,0);return RV_REFUSED;}
 v->attempted=1;v->recovery_active=1;v->recovery_phase=1;v->recovery_stage=1;
 uint64_t n=now(v);v->recovery_start=n;
 v->recovery_parent=d>n&&d-n>RV_RECOVERY_PARENT_MS?n+RV_RECOVERY_PARENT_MS:d;
 v->recovery_preimage=v->recovery_parent>n&&v->recovery_parent-n>RV_RECOVERY_PREIMAGE_MS?n+RV_RECOVERY_PREIMAGE_MS:v->recovery_parent;
 v->deadline=v->recovery_preimage;
 if(check(v,0,v->deadline)||(rc=recovery_config(v)))goto failed;
 v->recovery_stage=2;
 v->recovery_authorized=v->recovery;v->recovery_authorized_valid=1;
 rc=v->recovery_authorizer.consume(v->recovery_authorizer.user,&v->configuration,&v->recovery,v->deadline);
 if(rc||check(v,0,v->deadline)||(rc=recovery_config(v)))goto failed;
 v->confirmed=1;v->state=RV_PROVISIONING;
 struct rsp_recovery_owner owner={v,recovery_admit};v->recovery_stage=3;
 rc=rsp_recovery_prepare(&v->storage,&owner,v->recovery.preimage_sha256,v->recovery.control_digest,v->recovery.capture_firmware_digest,
    v->recovery.control_epoch,v->recovery.lease_high_water,v->deadline);
 if(rc)goto failed;
 v->recovery_stage=4;
 if(check(v,0,v->deadline)||(rc=recovery_config(v))||!rsp_recovery_quiescent(&v->storage)||
    atomic_load(&v->storage.gate)||v->recovery_transitions||check(v,0,v->deadline))goto failed;
 /* One explicit quiescent transition; P is checked before latching M. Parent
  * never changes, no subordinate operation is active or gets parent W. */
 n=now(v);if(n<v->last_now||n>=v->recovery_preimage||n>=v->recovery_parent)goto failed;
 v->last_now=n;v->recovery_transition_ms=n;v->recovery_transitions=1;v->recovery_phase=2;
 v->recovery_mutation=v->recovery_parent-n>RV_RECOVERY_MUTATION_MS?n+RV_RECOVERY_MUTATION_MS:v->recovery_parent;
 v->deadline=v->recovery_mutation;v->recovery_stage=5;
 rc=rsp_recovery_commit(&v->storage,v->deadline);if(rc)goto failed;
 v->recovery_stage=6;rc=applications(v,1);if(rc)goto failed;
 v->recovery_stage=7;rc=rsp_activate(&v->storage,v->deadline);if(rc)goto failed;
 v->recovery_stage=8;if(check(v,1,v->deadline)||(rc=rcfg_advance(RCFG_PROVISIONING,RCFG_ACTIVE)))goto failed;
 if((rc=configuration(v,RCFG_ACTIVE))||check(v,0,v->deadline))goto failed;
 v->recovery_stage=9;v->state=RV_WRITABLE;rc=done(v,0);v->recovery_active=0;return rc;
failed:
 (void)now(v);if(rc)v->recovery_rc=rc;rc=done(v,fence(v));v->recovery_active=0;return rc;
}
int rv_confirmation(struct recording_volume *v,uint8_t out[32])
{if(!v||!ext(v,out,32))return RV_ARGUMENT;int rc=lock(v);if(rc)return rc;
 if(v->fault||v->state!=RV_PREPARED)rc=RV_REFUSED;else memcpy(out,v->confirmation,32);atomic_store(&v->gate,0);return rc;}
int rv_provision(struct recording_volume *v,const uint8_t confirmation[32],uint64_t d)
{
 if(!v||!ext(v,confirmation,32))return RV_ARGUMENT;
 int rc=begin(v,d);if(rc)return rc;
 if(v->state!=RV_PREPARED||v->attempted||memcmp(confirmation,v->confirmation,32))return done(v,RV_REFUSED);
 if(configuration(v,RCFG_PREPARED)||v->hooks.confirm_provision(v->hooks.user,&v->configuration,confirmation,v->deadline)||check(v,1,v->deadline))return done(v,fence(v));
 v->attempted=1;
 if(rcfg_advance(RCFG_PREPARED,RCFG_PROVISIONING)||configuration(v,RCFG_PROVISIONING)||check(v,1,v->deadline))return done(v,fence(v));
 v->confirmed=1;v->state=RV_PROVISIONING;
 if(rsp_provision(&v->storage,v->deadline)||applications(v,1)||rsp_activate(&v->storage,v->deadline)||
    rcfg_advance(RCFG_PROVISIONING,RCFG_ACTIVE)||configuration(v,RCFG_ACTIVE))return done(v,fence(v));
 v->state=RV_WRITABLE;return done(v,0);
}
int rv_mount(struct recording_volume *v,uint64_t d)
{int rc=begin(v,d);if(rc)return rc;if(v->state!=RV_CONFIGURED||v->attempted)return done(v,RV_REFUSED);
 v->attempted=1;if(configuration(v,RCFG_ACTIVE))return done(v,RV_FAULT);v->state=RV_MOUNTING;
 if(rsp_resume(&v->storage,v->deadline)||applications(v,0))return done(v,fence(v));
 v->state=RV_READ_ONLY;return done(v,0);}
int rv_grant(struct recording_volume *v,uint64_t d)
{int rc=begin(v,d);if(rc)return rc;if(v->state!=RV_READ_ONLY||!root_ok(v))return done(v,RV_REFUSED);
 if(configuration(v,RCFG_ACTIVE)||rsp_grant(&v->storage,v->deadline))return done(v,fence(v));
 v->state=RV_WRITABLE;return done(v,0);}
int rv_close(struct recording_volume *v,uint64_t d)
{int rc=begin(v,d);if(rc)return rc;
 if((v->state!=RV_READ_ONLY&&v->state!=RV_WRITABLE)||v->store.status.job!=ROS_NONE||atomic_load(&v->metadata.gate))return done(v,RV_REFUSED);
 if(rsp_close(&v->storage,v->deadline))return done(v,fence(v));
 v->state=RV_CLOSED;return done(v,0);}
int rv_suspend(struct recording_volume *v,uint64_t d)
{int rc=begin(v,d);if(rc)return rc;
 if((v->state!=RV_READ_ONLY&&v->state!=RV_WRITABLE)||v->store.status.job!=ROS_NONE||atomic_load(&v->metadata.gate))return done(v,RV_REFUSED);
 if(rsp_suspend(&v->storage,v->deadline))return done(v,fence(v));
 v->state=RV_SUSPENDED;return done(v,0);}
int rv_reopen(struct recording_volume *v,uint64_t d)
{int rc=begin(v,d);if(rc)return rc;
 if(v->state!=RV_SUSPENDED||!root_ok(v))return done(v,RV_REFUSED);
 if(configuration(v,RCFG_ACTIVE))return done(v,RV_FAULT);
 v->state=RV_MOUNTING;
 if(rsp_reopen(&v->storage,v->deadline))return done(v,fence(v));
 v->state=RV_READ_ONLY;return done(v,0);}
int rv_access(struct recording_volume *v,struct ros_context **store,struct recording_sync_metadata **metadata,struct es_binding *binding)
{
 if(!v||!ext(v,store,sizeof(*store))||!ext(v,metadata,sizeof(*metadata))||!ext(v,binding,sizeof(*binding))||
 overlap(store,sizeof(*store),metadata,sizeof(*metadata))||overlap(store,sizeof(*store),binding,sizeof(*binding))||
 overlap(metadata,sizeof(*metadata),binding,sizeof(*binding)))return RV_ARGUMENT;
 int rc=lock(v);if(rc)return rc;
 if(v->fault||v->storage.fault||v->metadata.fault||v->store.status.fault||!root_ok(v)||
 (v->state!=RV_READ_ONLY&&v->state!=RV_WRITABLE))rc=RV_REFUSED;
 else{*store=&v->store;*metadata=&v->metadata;*binding=v->binding;}
 atomic_store(&v->gate,0);return rc;
}
