/* SPDX-License-Identifier: Apache-2.0 */
#include "recording_storage_provider.h"
#include <string.h>
#define RSP_MAGIC UINT32_C(0x52535031)
static void wipe(void *p,size_t n){volatile uint8_t *b=p;while(n--)*b++=0;}
static int overlap(const void *a,size_t an,const void *b,size_t bn)
{uintptr_t x=(uintptr_t)a,y=(uintptr_t)b;return an>UINTPTR_MAX-x||bn>UINTPTR_MAX-y||(x<y+bn&&y<x+an);}
static int external(const struct recording_storage_provider *s,const void *p,size_t n)
{return p&&!overlap(s,sizeof(*s),p,n);}
static int fault(struct recording_storage_provider *s){
 if(!s->first_fault.valid){struct rsp_first_fault *f=&s->first_fault;
  f->valid=1;f->stage=s->recovery_stage;f->route=s->route;f->row=s->route_row;
  f->opcode=s->last_opcode;f->bytes=s->last_bytes;f->fast=s->last_fast;f->transfer_rc=s->last_transfer_rc;
  f->observed_ms=s->observed_ms;f->deadline=s->deadline;f->epoch=s->journal.current.epoch;f->high_water=s->journal.current.high_water;
  f->programs=s->verified_programs;f->erases=s->verified_erases;f->phy_fault=s->phy.fault;f->stopped=s->phy.stopped;
  f->transfer_before_ms=s->last_transfer_before;f->transfer_after_ms=s->last_transfer_after;
  f->ready=s->phy.ready_known;f->a0=s->phy.a0;f->b0=s->phy.b0;f->c0=s->phy.status;f->media_flags=s->phy.media_flags;
  f->journal_fault=s->journal.fault;f->map_fault=s->map.fault;f->transfer=s->last_transfer;
 }
 s->fault=1;owned_dhara_revoke(&s->map);return RSP_FAULT;}
static uint64_t clock_now(void *u){struct recording_storage_provider *s=u;
 s->observed_ms=s->hal.now_ms(s->hal.user);return s->observed_ms;}
static int check(struct recording_storage_provider *s)
{uint64_t n=clock_now(s);if(s->fault||n<s->last_now||n>=s->deadline)return fault(s);s->last_now=n;return 0;}
static int begin(struct recording_storage_provider *s,uint64_t deadline)
{
 if(!s||s->magic!=RSP_MAGIC)return RSP_ARGUMENT;
 unsigned expected=0;if(!atomic_compare_exchange_strong(&s->gate,&expected,1))return RSP_BUSY;
 if(s->fault){atomic_store(&s->gate,0);return RSP_FAULT;}
 uint64_t now=clock_now(s);if(now<s->last_now||now>=deadline){fault(s);atomic_store(&s->gate,0);return RSP_FAULT;}
 s->deadline=deadline-now>120000U?now+120000U:deadline;s->last_now=now;return 0;
}
static int end(struct recording_storage_provider *s,int rc)
{
 if(!s->fault&&check(s))rc=RSP_FAULT;
 if(s->phy.fault||s->journal.fault||s->map.fault)rc=fault(s);
 /* PHY never uses these spans for DMA; unknown STOP only retains phy.tx/rx. */
 wipe(s->verify,sizeof(s->verify));wipe(s->raw,sizeof(s->raw));
 wipe(s->recovery_hash_input,sizeof(s->recovery_hash_input));s->route=0;
 atomic_store(&s->gate,0);return s->fault?RSP_FAULT:rc;
}
static int pause_job(struct recording_storage_provider *s)
{if(check(s)||s->policy.yield(s->policy.user,s->deadline)||check(s))return fault(s);return 0;}
static int policy(struct recording_storage_provider *s,uint32_t reason,uint32_t row)
{
 if(check(s)||s->policy.fault_latched(s->policy.user,s->journal.volume.digest,s->deadline))return fault(s);
 int rc=s->policy.admit(s->policy.user,reason,&s->journal.volume,s->journal.current.epoch,
                       row,s->phy.media_flags,s->deadline);
 if(check(s))return RSP_FAULT;
 return rc?RSP_REFUSED:0;
}
static int map_slot(struct recording_storage_provider *s,uint32_t row)
{for(unsigned i=0;i<32;i++)if(s->journal.volume.spec.map_blocks[i]==row/64U)return (int)i;return -1;}
static int control_bank(struct recording_storage_provider *s,uint32_t row)
{for(unsigned i=0;i<2;i++)if(s->journal.volume.spec.control_blocks[i]==row/64U)return (int)i;return -1;}
static int map_intent(struct recording_storage_provider *s,uint32_t kind,uint32_t row)
{
 const struct owned_dhara_lease *a=&s->journal.inflight,*b=&s->map.pending;
 return s->map.busy&&s->journal.map_inflight&&a->kind==kind&&a->physical_row==row&&
        a->map_slot<32&&a->map_slot==(uint32_t)map_slot(s,row)&&!memcmp(a,b,sizeof(*a))&&
        !memcmp(a->descriptor_digest,s->journal.volume.digest,32)&&a->lease_id&&
        !(s->journal.current.retired&(1U<<a->map_slot));
}
static int authorize(void *u,uint32_t access,uint32_t row,uint64_t deadline)
{
 struct recording_storage_provider *s=u;
 if(!atomic_load(&s->gate)||deadline!=s->deadline||s->route!=access||s->route_row!=row||check(s))return -1;
 if(access==NOP_READ)return policy(s,OCJ_ADMIT_BOOT,row);
 if(s->route_control){
  int bank=control_bank(s,row);if(bank<0||!s->journal.busy||s->journal.candidate.bank>1||s->journal.candidate.slot>63)return -1;
  if(access==NOP_ERASE){
   if(row%64U||(s->journal.current.epoch&&((uint32_t)bank==s->journal.current.bank||
      s->journal.candidate.bank!=(uint32_t)bank||s->journal.candidate.slot)))return -1;
  }else if(row!=s->journal.volume.spec.control_blocks[s->journal.candidate.bank]*64U+s->journal.candidate.slot)return -1;
 }else if(!map_intent(s,access==NOP_PROGRAM?OWNED_CONTROL_PROGRAM_PAGE:OWNED_CONTROL_ERASE_BLOCK,row))return -1;
 uint32_t reason=!s->route_control?OCJ_ADMIT_MAP:!s->journal.current.epoch?OCJ_ADMIT_PROVISION:
                 access==NOP_ERASE?OCJ_ADMIT_RECYCLE:OCJ_ADMIT_MAP;
 if(policy(s,reason,row)||s->policy.power(s->policy.user,access,row,deadline)||check(s))return -1;
 return 0;
}
static void wait_us(void *u,uint32_t us){struct recording_storage_provider *s=u;s->hal.wait_us(s->hal.user,us);}
static int hal_open(void *u,uint64_t d){struct recording_storage_provider *s=u;return s->hal.open(s->hal.user,d);}
static int hal_close(void *u,uint64_t d){struct recording_storage_provider *s=u;return s->hal.close(s->hal.user,d);}
static int transfer(void *u,const uint8_t *tx,uint8_t *rx,uint32_t n,uint32_t fast,uint64_t d,struct nop_transfer_result *r)
{struct recording_storage_provider *s=u;s->last_opcode=tx[0];s->last_bytes=n;s->last_fast=fast;
 s->last_transfer_before=clock_now(s);
 int rc=s->hal.transfer(s->hal.user,tx,rx,n,fast,d,r);s->last_transfer_after=clock_now(s);
 s->last_transfer_rc=rc;s->last_transfer=*r;return rc;}
static int route(struct recording_storage_provider *s,uint32_t access,uint32_t row,int control)
{if(s->route||check(s))return fault(s);s->route=access;s->route_row=row;s->route_control=(uint32_t)control;return 0;}
static int physical(struct recording_storage_provider *s,int rc)
{if(rc<0||check(s)){int failed=fault(s);s->route=0;return failed;}s->route=0;return rc;}
static int corrected(struct recording_storage_provider *s,uint32_t row,uint8_t *out,uint32_t *ecc)
{if(route(s,NOP_READ,row,control_bank(s,row)>=0))return -1;
 return physical(s,nand_owned_phy_read(&s->phy,row,out,ecc,s->deadline));}
static int raw_read(struct recording_storage_provider *s,uint32_t row,uint8_t *out)
{if(route(s,NOP_READ,row,control_bank(s,row)>=0))return -1;
 return physical(s,nand_owned_phy_raw(&s->phy,row,out,s->deadline));}
static int ecc_ok(uint32_t ecc){return ecc==0||ecc==1||ecc==3||ecc==5;}
static int control_admit(void *u,uint32_t reason,const uint8_t digest[32],uint32_t block,uint64_t epoch,uint64_t d)
{struct recording_storage_provider *s=u;if(d!=s->deadline||epoch!=s->journal.current.epoch||memcmp(digest,s->journal.volume.digest,32))return -1;
 return policy(s,reason,block==UINT32_MAX?UINT32_MAX:block*64U);}
static int latch_get(void *u)
{struct recording_storage_provider *s=u;return s->policy.fault_latched(s->policy.user,s->journal.volume.digest,s->deadline);}
static int latch_set(void *u,uint32_t bank,uint64_t d)
{struct recording_storage_provider *s=u;int rc=s->policy.latch_fault(s->policy.user,s->journal.volume.digest,bank,d);fault(s);return rc;}
static int control_read(void *u,uint32_t row,uint8_t *out,uint64_t d)
{
 struct recording_storage_provider *s=u;uint32_t ecc=UINT32_MAX;
 if(d!=s->deadline||control_bank(s,row)<0||out!=s->journal.rx||corrected(s,row,out,&ecc))return OCJ_IO_ERROR;
 if(ecc_ok(ecc))return 0;
 if(ecc!=2||raw_read(s,row,s->raw))return OCJ_IO_ERROR;
 memcpy(out,s->raw,4096);wipe(s->raw,sizeof(s->raw));return OCJ_IO_CHECKED_RAW;
}
static int control_raw(void *u,uint32_t row,uint8_t *out,uint64_t d)
{
 struct recording_storage_provider *s=u;
 if(d!=s->deadline||control_bank(s,row)<0||out!=s->journal.raw||!s->erase_scan_remaining||
    row!=s->erase_scan_row||raw_read(s,row,out))return OCJ_IO_ERROR;
 unsigned changed=0;for(unsigned i=0;i<4352;i++)changed|=(unsigned)(out[i]^0xffU);
 if(changed){int bank=control_bank(s,row);latch_set(s,(uint32_t)bank,d);return OCJ_IO_ERROR;}
 --s->erase_scan_remaining;++s->erase_scan_row;
 if(!s->erase_scan_remaining)++s->verified_erases;
 return pause_job(s)?OCJ_IO_ERROR:0;
}
static int erase_block(struct recording_storage_provider *s,uint32_t block,int control)
{
 uint32_t row=block*64U;
 if(route(s,NOP_ERASE,row,control))return -1;
 int rc=physical(s,nand_owned_phy_erase(&s->phy,block,s->deadline));if(rc)return rc;
 /* Fresh erase proof is additional to history; never FF-as-unused inference. */
 if(!control)for(unsigned p=0;p<64;p++){
  if(raw_read(s,row+p,s->raw))return -1;
  unsigned changed=0;for(unsigned i=0;i<4352;i++)changed|=(unsigned)(s->raw[i]^0xffU);
  wipe(s->raw,sizeof(s->raw));if(changed||pause_job(s))return fault(s);
 }
 if(control){s->erase_scan_row=row;s->erase_scan_remaining=64;}
 else ++s->verified_erases;
 return 0;
}
static int control_erase(void *u,uint32_t block,uint64_t d)
{struct recording_storage_provider *s=u;if(d!=s->deadline||block>=1536||control_bank(s,block*64U)<0)return OCJ_IO_ERROR;
 int rc=erase_block(s,block,1);return rc==NOP_MEDIA?OCJ_IO_MEDIA:rc?OCJ_IO_ERROR:0;}
static int program_page(struct recording_storage_provider *s,uint32_t row,const uint8_t *p,int control)
{
 if(route(s,NOP_PROGRAM,row,control))return -1;
 int rc=physical(s,nand_owned_phy_program(&s->phy,row,p,s->deadline));if(rc)return rc;
 uint32_t ecc=UINT32_MAX;
 if(corrected(s,row,s->verify,&ecc))return fault(s);
 if(!ecc_ok(ecc)||memcmp(p,s->verify,4096)){
  if(control)latch_set(s,(uint32_t)control_bank(s,row),s->deadline);
  return fault(s);
 }
 if(pause_job(s))return fault(s);
 wipe(s->verify,sizeof(s->verify));++s->verified_programs;return 0;
}
static int control_program(void *u,uint32_t row,const uint8_t *p,uint64_t d)
{struct recording_storage_provider *s=u;if(d!=s->deadline||control_bank(s,row)<0||p!=s->journal.tx)return OCJ_IO_ERROR;
 int rc=program_page(s,row,p,1);return rc==NOP_MEDIA?OCJ_IO_MEDIA:rc?OCJ_IO_ERROR:0;}
static int map_read(void *u,uint32_t row,uint8_t *p,uint32_t *ecc,uint64_t d)
{struct recording_storage_provider *s=u;return d==s->deadline&&map_slot(s,row)>=0&&p==s->map.main&&!corrected(s,row,p,ecc)?0:OD_IO_ERROR;}
static int map_raw(void *u,uint32_t row,uint8_t *p,uint64_t d)
{struct recording_storage_provider *s=u;return d==s->deadline&&map_slot(s,row)>=0&&p==s->map.raw&&!raw_read(s,row,p)?0:OD_IO_ERROR;}
static int map_program(void *u,uint32_t row,const uint8_t *p,uint64_t d)
{
 struct recording_storage_provider *s=u;int slot=map_slot(s,row);
 if(d!=s->deadline||slot<0||p!=s->map.main||!map_intent(s,OWNED_CONTROL_PROGRAM_PAGE,row))return OD_IO_ERROR;
 struct owned_page_binding b={0};struct owned_page_view view;
 memcpy(b.device_id,s->journal.volume.spec.device_id,16);memcpy(b.volume_id,s->journal.volume.spec.volume_id,16);
 b.generation=s->journal.volume.spec.generation;b.logical_page=(uint32_t)slot*64U+row%64U;
 b.physical_row=row;b.lease_id=s->journal.inflight.lease_id;b.kind=OWNED_PAGE_DATA;
 if(owned_page_validate(p,4096,&b,&s->journal.hash,&view)!=OWNED_PAGE_VALID||check(s))return OD_IO_ERROR;
 int rc=program_page(s,row,p,0);return rc==NOP_MEDIA?OD_IO_MEDIA:rc?OD_IO_ERROR:0;
}
static int map_erase(void *u,uint32_t block,uint64_t d)
{struct recording_storage_provider *s=u;if(d!=s->deadline||block>=1536||!map_intent(s,OWNED_CONTROL_ERASE_BLOCK,block*64U))return OD_IO_ERROR;
 int rc=erase_block(s,block,0);return rc==NOP_MEDIA?OD_IO_MEDIA:rc?OD_IO_ERROR:0;}
#define FORWARD(name,args,call) static int name args {struct recording_storage_provider *s=u;return call;}
FORWARD(capability,(void *u,const struct owned_dhara_capability *c,uint64_t d),ocj_validate_capability(&s->journal,c,d))
FORWARD(info,(void *u,uint32_t row,struct owned_dhara_page_info *i,uint64_t d),ocj_page_info(&s->journal,row,i,d))
FORWARD(acquire,(void *u,uint32_t kind,uint32_t slot,uint32_t row,struct owned_dhara_lease *l,uint64_t d),ocj_acquire(&s->journal,kind,slot,row,l,d))
FORWARD(resolve,(void *u,const struct owned_dhara_lease *l,uint64_t d),ocj_resolve(&s->journal,l,d))
FORWARD(retire,(void *u,uint32_t slot,uint32_t block,uint32_t bits,const struct owned_dhara_lease *l,uint64_t d),ocj_retire(&s->journal,slot,block,bits,l,d))
FORWARD(history,(void *u,uint64_t d),ocj_history_sync(&s->journal,d))
static int map_init(struct recording_storage_provider *s)
{
 struct owned_dhara_io io={s,clock_now,120000,capability,info,map_read,map_raw,map_program,map_erase,
                          acquire,resolve,retire,OD_LEASE_RESERVED_RANGE,history};
 uint8_t descriptor[512]; /* Canonical public metadata only, no private pages. */
 if(owned_volume_descriptor_build(descriptor,512,&s->journal.volume.spec,&s->journal.hash))return fault(s);
 int rc=owned_dhara_init(&s->map,descriptor,&s->journal.volume.spec,&s->journal.hash,&io,s->journal.current.retired);
 wipe(descriptor,sizeof(descriptor));return rc?fault(s):0;
}
int rsp_init(struct recording_storage_provider *s,const uint8_t descriptor[512],const struct owned_volume_spec *spec,
 const struct owned_page_hash *hash,const struct rsp_hal *hal,const struct rsp_policy *p)
{
 if(!s||s->magic||!external(s,descriptor,512)||!external(s,spec,sizeof(*spec))||!external(s,hash,sizeof(*hash))||
  !external(s,hal,sizeof(*hal))||!external(s,p,sizeof(*p))||!hal->now_ms||!hal->wait_us||!hal->open||!hal->transfer||!hal->close||
  !p->admit||!p->power||!p->fault_latched||!p->latch_fault||!p->yield||!atomic_is_lock_free(&s->gate))return RSP_ARGUMENT;
 struct ocj_io io={s,clock_now,control_admit,control_read,control_raw,control_erase,control_program,latch_get,latch_set};
 if(ocj_init(&s->journal,descriptor,spec,hash,&io))return RSP_ARGUMENT;
 s->hal=*hal;s->policy=*p;struct nop_port port={s,clock_now,wait_us,authorize,hal_open,transfer,hal_close};
 if(nand_owned_phy_init(&s->phy,&s->journal.volume,&port))return fault(s);
 s->magic=RSP_MAGIC;return 0;
}
static int open_phy(struct recording_storage_provider *s)
{
 if(policy(s,OCJ_ADMIT_BOOT,UINT32_MAX)||nand_owned_phy_open(&s->phy,s->deadline)||
    policy(s,OCJ_ADMIT_BOOT,UINT32_MAX))return fault(s);
 return 0;
}
int rsp_resume(struct recording_storage_provider *s,uint64_t d)
{
 int rc=begin(s,d);if(rc)return rc;if(s->attempted)return end(s,RSP_REFUSED);s->attempted=1;
 if(open_phy(s))return end(s,RSP_FAULT);
 rc=ocj_boot(&s->journal,s->deadline);if(rc==OCJ_NO_ROOT)return end(s,RSP_NO_ROOT);
 if(rc||map_init(s)||owned_dhara_resume(&s->map,s->deadline))return end(s,fault(s));
 s->ready=1;return end(s,0);
}
int rsp_provision(struct recording_storage_provider *s,uint64_t d)
{
 int rc=begin(s,d);if(rc)return rc;if(s->attempted)return end(s,RSP_REFUSED);s->attempted=1;
 if(policy(s,OCJ_ADMIT_PROVISION,UINT32_MAX)||open_phy(s)||ocj_provision(&s->journal,s->deadline)||map_init(s))return end(s,fault(s));
 struct owned_dhara_capability cap;if(ocj_capability(&s->journal,&cap)||owned_dhara_format(&s->map,&cap,s->deadline))return end(s,fault(s));
 s->ready=1;return end(s,0);
}
static void recovery_le(uint8_t *p,uint64_t v,unsigned n)
{for(unsigned i=0;i<n;i++)p[i]=(uint8_t)(v>>(8*i));}
static int recovery_hash(struct recording_storage_provider *s,const uint8_t *p,size_t n,uint8_t out[32])
{size_t actual=0;return s->journal.hash.sha256(s->journal.hash.user,p,n,out,32,&actual)||actual!=32||check(s)?fault(s):0;}
static int known_phase2_root(struct recording_storage_provider *s,const uint8_t digest[32],uint64_t epoch,uint64_t water)
{
 const struct ocj_snapshot *c=&s->journal.current;
 if(epoch!=5||water!=130||c->epoch!=epoch||c->high_water!=water||memcmp(c->digest,digest,32)||
    c->state!=OWNED_CONTROL_PROVISIONING||c->bank||c->slot!=4||c->retired||c->quarantine||c->open!=3||c->pending_kind)return 0;
 for(unsigned b=0;b<32;b++){
  const struct ocj_block *x=&c->blocks[b];
  if(x->valid||x->base!=(b==0?1U:b==1?66U:0U)||x->range_sequence!=(b==0?2U:b==1?4U:0U)||x->consumed!=(b<2?0U:64U))return 0;
 }
 return 1;
}
static int recovery_preimage(struct recording_storage_provider *s,const uint8_t expected[32],const uint8_t firmware[32])
{
 static const uint8_t domain[]="OpenPendant phase2 recovery preimage v1";
 uint8_t header[sizeof(domain)+116],previous[32],page_digest[32];
 const struct owned_volume_spec *v=&s->journal.volume.spec;uint8_t *p=header;
 memcpy(p,domain,sizeof(domain));p+=sizeof(domain);memcpy(p,v->device_id,16);p+=16;memcpy(p,v->volume_id,16);p+=16;
 recovery_le(p,v->generation,8);p+=8;memcpy(p,s->journal.volume.digest,32);p+=32;
 recovery_le(p,2,4);p+=4;recovery_le(p,2,8);p+=8;memcpy(p,firmware,32);
 int rc=recovery_hash(s,header,sizeof(header),previous);wipe(header,sizeof(header));
 for(uint32_t row=1025U*64U;!rc&&row<1059U*64U;row++){
  if(raw_read(s,row,s->raw)||recovery_hash(s,s->raw,sizeof(s->raw),page_digest)){rc=fault(s);break;}
  memcpy(s->recovery_hash_input,previous,32);recovery_le(s->recovery_hash_input+32,row,4);
  memcpy(s->recovery_hash_input+36,page_digest,32);
  rc=recovery_hash(s,s->recovery_hash_input,sizeof(s->recovery_hash_input),previous);
  wipe(s->raw,sizeof(s->raw));if(!rc&&pause_job(s))rc=fault(s);
 }
 if(!rc&&memcmp(previous,expected,32))rc=fault(s);
 wipe(previous,sizeof(previous));wipe(page_digest,sizeof(page_digest));return rc;
}
int rsp_recovery_quiescent(const struct recording_storage_provider *s)
{
 return s&&!s->fault&&!s->route&&!s->erase_scan_remaining&&!s->journal.busy&&!s->journal.map_inflight&&
  !s->journal.fault&&!s->map.busy&&!s->map.fault&&!s->map.writable&&!atomic_load(&s->phy.gate)&&
  s->phy.opened&&!s->phy.fault&&s->phy.stopped&&s->phy.ready_known&&s->phy.a0==0x7c&&s->phy.b0==0x10;
}
int rsp_recovery_prepare(struct recording_storage_provider *s,const struct rsp_recovery_owner *owner,const uint8_t preimage[32],
 const uint8_t control_digest[32],const uint8_t capture_firmware_digest[32],uint64_t epoch,uint64_t water,uint64_t d)
{
 if(!s||!external(s,owner,sizeof(*owner))||!owner->admit||!external(s,preimage,32)||!external(s,control_digest,32)||!external(s,capture_firmware_digest,32))return RSP_ARGUMENT;
 int rc=begin(s,d);if(rc)return rc;
 if(s->attempted)return end(s,RSP_REFUSED);
 s->attempted=1;
 s->recovery_owner=*owner;s->recovery_preimage_deadline=s->deadline;s->recovery_stage=1;
 if(s->recovery_owner.admit(s->recovery_owner.user,1,s->deadline)||check(s))return end(s,fault(s));
 /* No format, control erase or write can occur until ALL read-only checks pass.
  * The read scan uses the actual OCJ transition validator, not CP summaries. */
 if(open_phy(s))return end(s,fault(s));
 s->recovery_stage=2;if(ocj_boot(&s->journal,s->deadline)||!known_phase2_root(s,control_digest,epoch,water))return end(s,fault(s));
 s->recovery_stage=3;if(recovery_preimage(s,preimage,capture_firmware_digest)||!rsp_recovery_quiescent(s)||check(s))return end(s,fault(s));
 memcpy(s->recovery_control,control_digest,32);s->recovery_validated=1;return end(s,0);
}
int rsp_recovery_commit(struct recording_storage_provider *s,uint64_t d)
{
 int rc=begin(s,d);if(rc)return rc;
 if(!s->recovery_validated||s->recovery_commit_attempted||!s->recovery_owner.admit)return end(s,RSP_REFUSED);
 s->recovery_commit_attempted=1;s->recovery_stage=4;
 if(!rsp_recovery_quiescent(s)||!known_phase2_root(s,s->recovery_control,5,130)||
    s->recovery_owner.admit(s->recovery_owner.user,2,s->deadline)||check(s))return end(s,fault(s));
 /* Boot abandoned all tails. Existing reconciliation must publish to a fresh
  * alternate bank; never append to the FF-looking tail or erase the sole root.
  * Full map format takes NEW leases; no replay of epoch2/4 or ID1/66. */
 struct owned_dhara_capability cap;
 s->recovery_stage=5;if(ocj_reconcile(&s->journal,s->deadline)||map_init(s)||ocj_capability(&s->journal,&cap))return end(s,fault(s));
 s->recovery_stage=6;if(owned_dhara_format(&s->map,&cap,s->deadline))return end(s,fault(s));
 s->ready=1;return end(s,0);
}
int rsp_grant(struct recording_storage_provider *s,uint64_t d)
{
 int rc=begin(s,d);if(rc)return rc;if(!s->ready)return end(s,RSP_REFUSED);
 if(!s->journal.reconciled&&ocj_reconcile(&s->journal,s->deadline))return end(s,fault(s));
 struct owned_dhara_capability cap;if(ocj_capability(&s->journal,&cap)||owned_dhara_grant(&s->map,&cap,s->deadline))return end(s,fault(s));
 return end(s,0);
}
int rsp_activate(struct recording_storage_provider *s,uint64_t d)
{
 int rc=begin(s,d);if(rc)return rc;if(!s->ready)return end(s,RSP_REFUSED);
 struct owned_dhara_capability cap;
 if(ocj_activate(&s->journal,s->deadline)||ocj_capability(&s->journal,&cap)||owned_dhara_grant(&s->map,&cap,s->deadline))return end(s,fault(s));
 return end(s,0);
}
int rsp_read(struct recording_storage_provider *s,uint32_t sector,uint8_t out[2048],uint64_t d)
{if(!s||!external(s,out,2048))return RSP_ARGUMENT;int rc=begin(s,d);if(rc)return rc;
 if(!s->ready)return end(s,RSP_REFUSED);
 rc=owned_dhara_read(&s->map,sector,out,s->deadline);
 return end(s,rc==OD_MISSING?RSP_MISSING:rc?fault(s):0);}
int rsp_write(struct recording_storage_provider *s,uint32_t sector,const uint8_t in[2048],uint64_t d)
{if(!s||!external(s,in,2048))return RSP_ARGUMENT;int rc=begin(s,d);if(rc)return rc;
 if(!s->ready||!s->map.writable)return end(s,RSP_REFUSED);
 return end(s,owned_dhara_write(&s->map,sector,in,s->deadline)?fault(s):0);}
int rsp_sync(struct recording_storage_provider *s,uint64_t d)
{int rc=begin(s,d);if(rc)return rc;if(!s->ready||!s->map.writable)return end(s,RSP_REFUSED);
 return end(s,owned_dhara_sync(&s->map,s->deadline)?fault(s):0);}
int rsp_capacity(struct recording_storage_provider *s,uint32_t *out)
{if(!s||s->magic!=RSP_MAGIC||!external(s,out,sizeof(*out)))return RSP_ARGUMENT;
 unsigned expected=0;if(!atomic_compare_exchange_strong(&s->gate,&expected,1))return RSP_BUSY;
 int rc=s->fault||!s->ready?RSP_REFUSED:owned_dhara_capacity(&s->map,out)?RSP_FAULT:0;
 atomic_store(&s->gate,0);return rc;}
int rsp_close(struct recording_storage_provider *s,uint64_t d)
{int rc=begin(s,d);if(rc)return rc;owned_dhara_revoke(&s->map);s->ready=0;
 return end(s,nand_owned_phy_close(&s->phy,s->deadline)?fault(s):0);}
int rsp_suspend(struct recording_storage_provider *s,uint64_t d)
{
 int rc=begin(s,d);if(rc)return rc;
 if(!s->ready||s->suspended||s->map.busy||s->journal.busy||s->journal.map_inflight)return end(s,RSP_REFUSED);
 if(s->map.writable&&owned_dhara_sync(&s->map,s->deadline))return end(s,fault(s));
 owned_dhara_revoke(&s->map);
 if(nand_owned_phy_close(&s->phy,s->deadline))return end(s,fault(s));
 s->ready=0;s->suspended=1;return end(s,0);
}
int rsp_reopen(struct recording_storage_provider *s,uint64_t d)
{
 int rc=begin(s,d);if(rc)return rc;
 if(s->ready||!s->suspended||!s->attempted||!s->map.ready||s->map.writable||s->phy.opened)return end(s,RSP_REFUSED);
 if(open_phy(s))return end(s,RSP_FAULT);
 s->suspended=0;s->ready=1;return end(s,0);
}
