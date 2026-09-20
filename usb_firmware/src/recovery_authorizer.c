#include "recovery_authorizer.h"
#include <string.h>
#define RA_MAGIC UINT32_C(0x52415531)
static void wipe(void *p,size_t n){volatile uint8_t *b=p;while(n--)*b++=0;}
static int outside(const struct recovery_authorizer *a,const void *p,size_t n)
{uintptr_t x=(uintptr_t)a,y=(uintptr_t)p;return p&&x<=UINTPTR_MAX-sizeof(*a)&&y<=UINTPTR_MAX-n&&!(x<y+n&&y<x+sizeof(*a));}
static int id(const uint8_t *p,size_t n){unsigned z=0,f=0;while(n--){z|=*p;f|=*p++^255U;}return z&&f;}
static void le(uint8_t *p,uint64_t v,unsigned n){for(unsigned i=0;i<n;i++)p[i]=(uint8_t)(v>>(8*i));}
static int digest(struct recovery_authorizer *a,const uint8_t *p,size_t n,uint8_t out[32])
{size_t actual=0;return a->hash.sha256(a->hash.user,p,n,out,32,&actual)||actual!=32?RA_FAULT:0;}
static int build(struct recovery_authorizer *a,const struct recording_configuration *c,const struct rv_recovery_proof *p)
{
 const struct ra_evidence *e=&a->evidence;struct owned_volume_decoded decoded;
 if(c->phase!=RCFG_PROVISIONING||c->revision!=2||c->fault_banks||c->spec.generation!=1||
    memcmp(c->spec.device_id,e->expected_device,16)||memcmp(c->spec.preservation_manifest_sha256,e->original_manifest,32)||
    p->control_epoch!=5||p->lease_high_water!=130||c->recipient[0]!=4||
    !id(p->transaction,16)||!id(p->configuration_sha256,32)||!id(p->preimage_sha256,32)||
    !id(p->control_digest,32)||!id(p->capture_firmware_digest,32))return RA_REFUSED;
 for(unsigned i=0;i<32;i++)if(c->spec.map_blocks[i]!=1025U+i)return RA_REFUSED;
 if(c->spec.control_blocks[0]!=1057||c->spec.control_blocks[1]!=1058||
    owned_volume_descriptor_validate(c->descriptor,512,&c->spec,&a->hash,&decoded))return RA_REFUSED;
 static const uint8_t cd[]="OpenPendant phase2 recovery configuration v1";
 static const uint8_t kd[]="OpenPendant recipient public key v1";
 uint8_t bytes[sizeof(cd)+593],sum[32];int rc;
 memcpy(bytes,kd,sizeof(kd));bytes[sizeof(kd)]=0;bytes[sizeof(kd)+1]=16;memcpy(bytes+sizeof(kd)+2,c->recipient,65);
 rc=digest(a,bytes,sizeof(kd)+67,sum);
 if(!rc&&memcmp(sum,c->spec.recipient_fingerprint,32))rc=RA_REFUSED;
 if(!rc){
  memcpy(bytes,cd,sizeof(cd));memcpy(bytes+sizeof(cd),c->descriptor,512);memcpy(bytes+sizeof(cd)+512,c->recipient,65);
  le(bytes+sizeof(cd)+577,c->revision,8);le(bytes+sizeof(cd)+585,c->phase,4);le(bytes+sizeof(cd)+589,c->fault_banks,4);
  rc=digest(a,bytes,sizeof(bytes),sum);if(!rc&&memcmp(sum,p->configuration_sha256,32))rc=RA_REFUSED;
 }
 wipe(bytes,sizeof(bytes));wipe(sum,sizeof(sum));if(rc)return rc;
 uint8_t *r=a->work;memset(r,0,RA_RECORD_BYTES);memcpy(r,"OPNDRA1",7);le(r+8,1,2);le(r+10,RA_RECORD_BYTES,2);le(r+12,1,4);
 memcpy(r+16,e->expected_device,16);memcpy(r+32,p->transaction,16);memcpy(r+48,p->configuration_sha256,32);
 memcpy(r+80,p->preimage_sha256,32);memcpy(r+112,p->control_digest,32);memcpy(r+144,p->capture_firmware_digest,32);
 le(r+176,p->control_epoch,8);le(r+184,p->lease_high_water,8);
 memcpy(r+192,e->current_manifest,32);memcpy(r+224,e->original_manifest,32);memcpy(r+256,e->control_scan,32);
 memcpy(r+288,c->descriptor,512);memcpy(r+800,c->recipient,65);le(r+865,c->revision,8);le(r+873,c->phase,4);le(r+877,c->fault_banks,4);
 return digest(a,r,RA_DIGEST_OFFSET,r+RA_DIGEST_OFFSET);
}
static int check(struct recovery_authorizer *a)
{uint64_t t=a->port.now_ms(a->port.user);if(t<a->last_now||t>=a->deadline)return RA_FAULT;a->last_now=t;return 0;}
int ra_init(struct recovery_authorizer *a,const struct ra_port *p,const struct owned_page_hash *h,
 const struct recording_configuration *c,const struct rv_recovery_proof *proof,const struct ra_evidence *e)
{
 if(!a||!outside(a,p,sizeof(*p))||!outside(a,h,sizeof(*h))||!outside(a,c,sizeof(*c))||!outside(a,proof,sizeof(*proof))||
    !outside(a,e,sizeof(*e))||a->magic||!h->sha256||!p->now_ms||!p->acquire||!p->release||!p->consume_permit||!p->read||!p->write||
    !id(e->expected_device,16)||!id(e->current_manifest,32)||!id(e->original_manifest,32)||!id(e->control_scan,32)||
    !atomic_is_lock_free(&a->gate))return RA_ARGUMENT;
 a->magic=RA_MAGIC;a->port=*p;a->hash=*h;a->evidence=*e;
 int rc=build(a,c,proof);if(rc)a->fault=1;else memcpy(a->expected,a->work,RA_RECORD_BYTES);
 wipe(a->work,RA_RECORD_BYTES);return rc;
}
int ra_consume(void *u,const struct recording_configuration *c,const struct rv_recovery_proof *p,uint64_t d)
{
 struct recovery_authorizer *a=u;
 if(!a||a->magic!=RA_MAGIC||!outside(a,c,sizeof(*c))||!outside(a,p,sizeof(*p)))return RA_ARGUMENT;
 unsigned zero=0;if(!atomic_compare_exchange_strong(&a->gate,&zero,1))return RA_BUSY;
 int rc=RA_REFUSED;size_t actual=0;
 /* A prior ambiguous release retains ownership. Refusal must not retry even
  * cleanup callbacks merely because the caller invokes consume again. */
 if(a->attempted||a->fault){atomic_store(&a->gate,0);return RA_REFUSED;}
 a->attempted=1;
 uint64_t t=a->port.now_ms(a->port.user);a->last_now=t;a->deadline=d>t&&d-t>RA_MAX_MS?t+RA_MAX_MS:d;
 if(check(a)||build(a,c,p)||memcmp(a->expected,a->work,RA_RECORD_BYTES)||check(a)){rc=RA_FAULT;goto finished;}
 if(a->port.acquire(a->port.user)){rc=RA_BUSY;goto finished;}a->held=1;
 /* Consume independent permit BEFORE any ambiguous settings read/write. It is
  * the durable barrier when damaged/never-committed NVS bytes look absent. */
 if(check(a)||a->port.consume_permit(a->port.user,p->transaction,a->expected+RA_DIGEST_OFFSET,a->deadline)||check(a)){
  rc=RA_FAULT;goto finished;
 }
 wipe(a->work,RA_RECORD_BYTES);
 rc=a->port.read(a->port.user,a->work,&actual,a->deadline);
 if(check(a)||rc!=RA_ABSENT||actual){rc=RA_REFUSED;goto finished;}
 if(a->port.write(a->port.user,a->expected,a->deadline)||check(a)){rc=RA_FAULT;goto finished;}
 actual=0;wipe(a->work,RA_RECORD_BYTES);
 if(a->port.read(a->port.user,a->work,&actual,a->deadline)||check(a)||actual!=RA_RECORD_BYTES||
    memcmp(a->work,a->expected,RA_RECORD_BYTES)){rc=RA_FAULT;goto finished;}
 rc=0;
finished:
 if(a->held){if(a->port.release(a->port.user)){a->fault=1;rc=RA_FAULT;}else a->held=0;}
 if(!rc&&check(a))rc=RA_FAULT;
 if(rc)a->fault=1;
 wipe(a->work,RA_RECORD_BYTES);atomic_store(&a->gate,0);return rc;
}
