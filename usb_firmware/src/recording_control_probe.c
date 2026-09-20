/* SPDX-License-Identifier: Apache-2.0 */
#include "recording_control_probe.h"
#include <limits.h>
#include <string.h>
static const uint8_t fixed_device[16]={0x6a,0x5f,0x3e,0xaf,0xbb,0x97,0x51,0xfb,0xa9,0x93,0xe2,0x8d,0xcf,0x2b,0x87,0x10};
static const uint8_t fixed_volume[16]={0x65,0xdb,0xaf,0x95,0x2c,0x66,0x4f,0x48,0xa1,0x3f,0xd2,0x2a,0x18,0x13,0x63,0xab};
static const uint8_t fixed_descriptor[32]={0x11,0xc7,0xcf,0x70,0x13,0xbd,0x8e,0xfe,0x7d,0x17,0x63,0xad,0xef,0xa0,0x60,0x62,0xc5,0xc8,0x37,0x29,0xf3,0x15,0x80,0x5b,0xfc,0xca,0xda,0x49,0x4a,0x87,0xaf,0x8b};
_Static_assert(CP_ROWS==128 && CP_RECORD_BYTES==200 && CP_MAX_STARTS==2697,"Closed diagnostic bounds");
static void wipe(void *p,size_t n){volatile uint8_t *v=p;while(n--)*v++=0;}
static int overlap(const void *a,size_t an,const void *b,size_t bn)
{uintptr_t x=(uintptr_t)a,y=(uintptr_t)b;return an>UINTPTR_MAX-x||bn>UINTPTR_MAX-y||(x<y+bn&&y<x+an);}
static int external(const struct control_probe *c,const void *p,size_t n)
{return p&&!overlap(c,sizeof(*c),p,n);}
static void u32(uint8_t *p,uint32_t x){for(unsigned i=0;i<4;i++)p[i]=(uint8_t)(x>>(8*i));}
static void u64(uint8_t *p,uint64_t x){for(unsigned i=0;i<8;i++)p[i]=(uint8_t)(x>>(8*i));}
static uint64_t clock_now(struct control_probe *c)
{uint64_t n=c->port.now_ms(c->port.user);if(n<c->last_now)c->backwards=1;else c->last_now=n;return n;}
static int check(struct control_probe *c,uint64_t d)
{
 uint64_t n=clock_now(c);if(c->backwards||n>=d||n>=c->deadline)return CP_TIME;
 if(c->port.admit(c->port.user,fixed_device,fixed_descriptor,c->deadline))return CP_ADMISSION;
 n=clock_now(c);return c->backwards||n>=d||n>=c->deadline?CP_TIME:0;
}
static void scrub(struct control_probe *c)
{if(c->stopped){wipe(c->tx,sizeof(c->tx));wipe(c->rx,sizeof(c->rx));wipe(c->control_rx,sizeof(c->control_rx));c->scrubbed=1;}}
static int xfer(struct control_probe *c,uint32_t n,int cache,uint64_t d)
{
 int rc=check(c,d);if(rc)return rc;
 if(!c->open||!c->stopped||c->starts>=CP_MAX_STARTS)return CP_TRANSFER;
 struct cp_transfer r={0};uint8_t *rx=cache?c->rx:c->control_rx;
 memset(rx,0,n);c->scrubbed=0;
 /* A PAGE_READ START makes readiness unknown even if the HAL returns error. */
 int loading=c->tx[0]==0x13;
 rc=c->port.transfer(c->port.user,c->tx,rx,n,(uint32_t)cache,d,&r);
 if(r.started==1)++c->starts;
 if(loading&&r.started!=0)c->ready=0;
 c->result.transport_rc=rc;c->stopped=r.stopped==1;
 if(!c->stopped)return CP_STOP_UNKNOWN;
 if(rc||r.started!=1||r.tx_bytes!=n||r.rx_bytes!=n)return CP_TRANSFER;
 return check(c,d);
}
static int get(struct control_probe *c,uint8_t reg,uint32_t *value,uint64_t d)
{c->tx[0]=0x0f;c->tx[1]=reg;c->tx[2]=0;int rc=xfer(c,3,0,d);if(!rc)*value=c->control_rx[2];return rc;}
static int baseline(struct control_probe *c,uint64_t d)
{
 c->result.last_stage=2;
 int rc=get(c,0xc0,&c->result.c0,d);if(rc)return rc;
 c->ready=(c->result.c0&0x81U)==0;
 if((c->result.c0&0x8fU)!=0)return CP_BASELINE;
 if((rc=get(c,0xa0,&c->result.a0,d))||(rc=get(c,0xb0,&c->result.b0,d)))return rc;
 return c->result.a0==0x7c&&c->result.b0==0x10?0:CP_BASELINE;
}
static int prepare(struct control_probe *c)
{
 for(unsigned i=0;i<3;i++){
  c->result.last_stage=1;c->tx[0]=0x9f;c->tx[1]=c->tx[2]=c->tx[3]=0;
  int rc=xfer(c,4,0,c->deadline);if(rc)return rc;
  if(c->control_rx[2]!=0x2c||c->control_rx[3]!=0x35)return CP_BASELINE;
 }
 return baseline(c,c->deadline);
}
static int page(struct control_probe *c,uint32_t index)
{
 uint32_t bank=index/64U,slot=index%64U,row=(1057U+bank)*64U+slot;
 uint64_t n=clock_now(c),d=n>UINT64_MAX-1000U?UINT64_MAX:n+1000U;
 if(d>c->deadline)d=c->deadline;
 int rc=check(c,d);if(rc)return rc;
 c->result.last_row=row;c->result.last_stage=3;
 c->tx[0]=0x13;c->tx[1]=(uint8_t)(row>>16);c->tx[2]=(uint8_t)(row>>8);c->tx[3]=(uint8_t)row;
 if((rc=xfer(c,4,0,d)))return rc;
 c->result.last_stage=4;
 n=clock_now(c);uint64_t ready_end=n>UINT64_MAX-10U?UINT64_MAX:n+10U;
 if(ready_end>d)ready_end=d;
 for(unsigned poll=0;poll<16;poll++){
  ++c->result.polls;if((rc=get(c,0xc0,&c->result.c0,ready_end)))return rc;
  if(c->result.c0&0x80U)return CP_BASELINE;
  if(!(c->result.c0&1U)){c->ready=1;break;}
  c->port.pause_us(c->port.user,100);if((rc=check(c,ready_end)))return rc;
 }
 if(!c->ready)return CP_TIME;
 if(c->result.c0&0x8fU)return CP_BASELINE;
 uint32_t ready_c0=c->result.c0;c->result.ecc=(ready_c0>>4)&7U;
 if(c->result.ecc!=0&&c->result.ecc!=1&&c->result.ecc!=3&&c->result.ecc!=5)return CP_ECC;
 c->result.last_stage=5;memset(c->tx,0,sizeof(c->tx));c->tx[0]=0x03;
 if((rc=xfer(c,4100,1,d))||(rc=baseline(c,d)))return rc;
 if(c->result.c0!=ready_c0)return CP_BASELINE;
 ++c->result.rows;c->result.last_stage=6;
 memset(c->metadata,0,sizeof(c->metadata));memcpy(c->metadata,"OPNDCP1",7);
 u32(c->metadata+8,1);u32(c->metadata+12,index);u32(c->metadata+16,bank);
 u32(c->metadata+20,slot);u32(c->metadata+24,row);u32(c->metadata+28,c->result.ecc);
 size_t actual=0;
 if(c->hash.sha256(c->hash.user,c->rx+4,4096,c->metadata+104,32,&actual)||actual!=32)return CP_HASH;
 if((rc=check(c,d)))return rc;
 rc=ocj_decode(&c->volume,bank,slot,&c->hash,c->rx+4,&c->decoded);
 if(rc!=0&&rc!=1&&rc!=OCJ_CONFLICT)return CP_HASH;
 uint32_t cls=rc==0?CP_CANONICAL:rc==1?CP_NOT_VALID:CP_CONFLICT;
 u32(c->metadata+32,cls);
 if(cls==CP_CANONICAL){
  const struct ocj_snapshot *s=&c->decoded;
  uint32_t broken=c->gap[bank]||(!slot?s->slot!=0:
   c->previous_epoch[bank]==0||s->previous_epoch!=c->previous_epoch[bank]||memcmp(s->previous_digest,c->previous_digest[bank],32));
  u32(c->metadata+36,broken);c->result.predecessor_breaks+=broken;
  u64(c->metadata+40,s->epoch);u64(c->metadata+48,s->previous_epoch);u32(c->metadata+56,s->state);
  u32(c->metadata+60,s->pending_kind);u32(c->metadata+64,s->pending_slot);
  uint32_t pending_row=s->pending_kind?c->volume.spec.map_blocks[s->pending_slot]*64U:UINT32_MAX;
  u32(c->metadata+68,pending_row);u64(c->metadata+72,s->pending_lease);u64(c->metadata+80,s->high_water);
  u32(c->metadata+88,s->retired);u32(c->metadata+92,s->quarantine);u32(c->metadata+96,s->open);
  memcpy(c->metadata+136,s->digest,32);memcpy(c->metadata+168,s->previous_digest,32);
  struct cp_highest *h=&c->result.highest[bank];
  if(s->epoch>h->epoch){h->epoch=s->epoch;h->previous_epoch=s->previous_epoch;h->pending_lease=s->pending_lease;
   h->bank=bank;h->slot=slot;h->state=s->state;h->pending_kind=s->pending_kind;h->pending_slot=s->pending_slot;
   h->pending_row=pending_row;memcpy(h->record_digest,s->digest,32);}
  c->previous_epoch[bank]=s->epoch;memcpy(c->previous_digest[bank],s->digest,32);++c->result.canonical;
 }else{c->gap[bank]=1;if(cls==CP_NOT_VALID)++c->result.not_valid;else ++c->result.conflicts;}
 scrub(c);wipe(&c->decoded,sizeof(c->decoded));
 if((rc=check(c,d)))return rc;
 c->result.last_stage=7;n=clock_now(c);uint64_t event_end=n>UINT64_MAX-2000U?UINT64_MAX:n+2000U;
 if(event_end>c->deadline)event_end=c->deadline;
 if(c->port.record(c->port.user,c->metadata,event_end))return CP_OBSERVER;
 if((rc=check(c,event_end)))return rc;
 ++c->result.records;wipe(c->metadata,sizeof(c->metadata));return 0;
}
int control_probe_run(struct control_probe *c,const uint8_t descriptor[512],const struct owned_volume_spec *expected,
 const struct owned_page_hash *hash,const struct cp_port *port,uint64_t deadline,struct cp_result *out)
{
 if(!c||!external(c,descriptor,512)||!external(c,expected,sizeof(*expected))||!external(c,hash,sizeof(*hash))||
 !external(c,port,sizeof(*port))||!external(c,out,sizeof(*out))||!hash->sha256||!port->now_ms||!port->admit||
 !port->open||!port->transfer||!port->close||!port->pause_us||!port->record||!atomic_is_lock_free(&c->gate))return CP_ARGUMENT;
 if(overlap(out,sizeof(*out),descriptor,512)||overlap(out,sizeof(*out),expected,sizeof(*expected))||
 overlap(out,sizeof(*out),hash,sizeof(*hash))||overlap(out,sizeof(*out),port,sizeof(*port)))return CP_ARGUMENT;
 unsigned available=0;if(!atomic_compare_exchange_strong(&c->gate,&available,1))return CP_BUSY;
 if(c->used){atomic_store(&c->gate,0);return CP_CONSUMED;}c->used=1;
 c->port=*port;c->hash=*hash;c->stopped=c->scrubbed=1;c->start=c->last_now=port->now_ms(port->user);
 c->deadline=deadline>c->start&&deadline-c->start>CP_BUDGET_MS?c->start+CP_BUDGET_MS:deadline;
 c->result.attempted=1;c->result.last_row=UINT32_MAX;
 int rc=CP_ADMISSION;
 if(owned_volume_descriptor_validate(descriptor,512,expected,hash,&c->volume)||
 memcmp(c->volume.digest,fixed_descriptor,32)||memcmp(expected->device_id,fixed_device,16)||
 memcmp(expected->volume_id,fixed_volume,16)||expected->generation!=1||
 expected->control_blocks[0]!=1057||expected->control_blocks[1]!=1058)goto finish;
 for(unsigned i=0;i<32;i++)if(expected->map_blocks[i]!=1025U+i)goto finish;
 if((rc=check(c,c->deadline)))goto finish;
 c->open=1; /* Opening failure may have acquired ownership; never guess. */
 if(c->port.open(c->port.user,c->deadline)){rc=CP_ADMISSION;goto finish;}
 c->result.opened=1;
 if((rc=check(c,c->deadline))||(rc=prepare(c)))goto finish;
 for(unsigned i=0;i<128;i++)if((rc=page(c,i)))goto finish;
 rc=baseline(c,c->deadline);if(!rc)c->result.complete=1;
finish:
 /* No NAND recovery commands, even on baseline mismatch or timeout. */
 if(c->stopped)scrub(c);
 if(c->open&&c->stopped&&c->ready){
  uint64_t n=clock_now(c),close_end=n>UINT64_MAX-20U?UINT64_MAX:n+20U;
  if(!rc)c->result.last_stage=8;
  c->result.close_rc=c->port.close(c->port.user,close_end);
  uint64_t after=clock_now(c);
  if(!c->result.close_rc&&!c->backwards&&after<close_end){c->open=0;c->result.released=1;}
  else{if(!c->result.close_rc)c->result.close_rc=CP_TIME;if(!rc)rc=CP_CLOSE;}
 }
 if(c->open)c->result.retained=1;
 c->result.rc=rc;c->result.starts=c->starts;c->result.stopped=c->stopped;c->result.ready_known=c->ready;
 c->result.scrubbed=c->scrubbed;c->result.elapsed_ms=c->last_now>=c->start?c->last_now-c->start:0;
 if(rc)c->result.complete=0;
 if(c->stopped){wipe(c->metadata,sizeof(c->metadata));wipe(&c->decoded,sizeof(c->decoded));}
 *out=c->result;atomic_store(&c->gate,0);return rc;
}
