#include "recording_current_preimage.h"
#include <limits.h>
#include <string.h>
#define CPI_MAGIC UINT32_C(0x43504931)
static const uint8_t cpi_device[16]={0x6a,0x5f,0x3e,0xaf,0xbb,0x97,0x51,0xfb,0xa9,0x93,0xe2,0x8d,0xcf,0x2b,0x87,0x10};
static const uint8_t cpi_volume[16]={0x65,0xdb,0xaf,0x95,0x2c,0x66,0x4f,0x48,0xa1,0x3f,0xd2,0x2a,0x18,0x13,0x63,0xab};
static const uint8_t cpi_digest[32]={0x11,0xc7,0xcf,0x70,0x13,0xbd,0x8e,0xfe,0x7d,0x17,0x63,0xad,0xef,0xa0,0x60,0x62,0xc5,0xc8,0x37,0x29,0xf3,0x15,0x80,0x5b,0xfc,0xca,0xda,0x49,0x4a,0x87,0xaf,0x8b};
static void cpi_wipe(void *p,size_t n){volatile uint8_t *b=p;while(n--)*b++=0;}
static int cpi_outside(const struct current_preimage *c,const void *p,size_t n)
{uintptr_t a=(uintptr_t)c,b=(uintptr_t)p;return p&&b<=UINTPTR_MAX-n&&a<=UINTPTR_MAX-sizeof(*c)&&!(a<b+n&&b<a+sizeof(*c));}
static uint64_t cpi_now(void *u){struct current_preimage *c=u;return c->actual.now_ms(c->actual.user);}
static int cpi_guard(struct current_preimage *c,uint64_t d)
{
 uint64_t t=cpi_now(c);
 if(d!=c->deadline||t<c->last_now||t>=d)return CPI_TIME;
 c->last_now=t;
 if(c->owner.check(c->owner.user,d))return CPI_ADMISSION;
 t=cpi_now(c);if(t<c->last_now||t>=d)return CPI_TIME;c->last_now=t;return 0;
}
static void cpi_failure(struct current_preimage *c,int rc,uint32_t op,uint32_t n,uint32_t fast,
 const struct nop_transfer_result *r,uint64_t before,uint32_t cycle)
{
 if(c->result.first.valid)return;
 struct cpi_fault *f=&c->result.first;f->valid=1;f->stage=c->stage;f->rc=rc;
 f->opcode=op;f->row=c->result.row;f->bytes=n;f->fast=fast;f->before_ms=before;f->after_ms=cpi_now(c);
 f->elapsed_cycles=c->owner.cycles(c->owner.user)-cycle;f->cycle_hz=c->owner.cycle_hz;
 if(r){f->started=r->started;f->stopped=r->stopped;f->tx_amount=r->tx_amount;f->rx_amount=r->rx_amount;}
}
static int cpi_read_stage(const struct current_preimage *c){return c->stage==CPI_FIRST||c->stage==CPI_SECOND;}
static int cpi_shape(struct current_preimage *c,const uint8_t *tx,uint32_t n,uint32_t fast)
{
 if(tx!=c->phy.tx||fast>1)return 0;
 if(n==4&&!fast&&tx[0]==0x9f)return c->stage==CPI_OPEN&&!tx[1]&&!tx[2]&&!tx[3];
 if(n==3&&!fast&&tx[0]==0x0f)return !tx[2]&&(tx[1]==0xa0||tx[1]==0xb0||tx[1]==0xc0);
 if(!cpi_read_stage(c))return 0;
 if(n==3&&!fast&&tx[0]==0x1f&&tx[1]==0xb0)
  return (tx[2]==0&&c->raw_phase==0)||(tx[2]==0x10&&c->raw_phase==3);
 if(n==4&&!fast&&tx[0]==0x13){uint32_t row=((uint32_t)tx[1]<<16)|((uint32_t)tx[2]<<8)|tx[3];
  return row==c->result.row&&row>=65600U&&row<=67775U&&c->raw_phase==1;}
 if(n==4356&&fast&&tx[0]==3&&c->raw_phase==2){
  unsigned nonzero=0;for(unsigned i=1;i<n;++i)nonzero|=tx[i];return !nonzero;}
 return 0;
}
static int cpi_transfer(void *u,const uint8_t *tx,uint8_t *rx,uint32_t n,uint32_t fast,uint64_t d,struct nop_transfer_result *r)
{
 struct current_preimage *c=u;memset(r,0,sizeof(*r));uint64_t before=cpi_now(c);uint32_t cycle=c->owner.cycles(c->owner.user);
 uint32_t op=tx==c->phy.tx?tx[0]:UINT32_MAX;int rc=cpi_guard(c,d);
 if(!rc&&rx==c->phy.rx&&cpi_shape(c,tx,n,fast)){
  uint32_t set_value=op==0x1f?tx[2]:UINT32_MAX;
  rc=c->actual.transfer(c->actual.user,tx,rx,n,fast,d,r);
  if(r->started==1){if(set_value==0)++c->result.off_starts;if(set_value==0x10)++c->result.on_starts;}
  if(!rc&&(r->started!=1||r->stopped!=1||r->tx_amount!=n||r->rx_amount!=n))rc=CPI_SHAPE;
  if(!rc)rc=cpi_guard(c,d);
  if(!rc){if(op==0x1f)c->raw_phase=set_value==0?1U:4U;else if(op==0x13)c->raw_phase=2;else if(op==3)c->raw_phase=3;}
 }else if(!rc){r->stopped=c->phy.stopped;rc=CPI_SHAPE;}
 if(rc)cpi_failure(c,rc,op,n,fast,r,before,cycle);
 return rc;
}
static void cpi_wait(void *u,uint32_t us){struct current_preimage *c=u;c->actual.wait_us(c->actual.user,us);}
static int cpi_authorize(void *u,uint32_t access,uint32_t row,uint64_t d)
{
 struct current_preimage *c=u;int rc=cpi_guard(c,d);
 if(rc||access!=NOP_READ||!cpi_read_stage(c)||row!=c->result.row)return rc?rc:CPI_ADMISSION;
 rc=c->actual.authorize(c->actual.user,access,row,d);return rc?rc:cpi_guard(c,d);
}
static int cpi_open(void *u,uint64_t d)
{struct current_preimage *c=u;int rc=cpi_guard(c,d);if(!rc)rc=c->actual.open(c->actual.user,d);return rc?rc:cpi_guard(c,d);}
static int cpi_close(void *u,uint64_t d)
{struct current_preimage *c=u;int rc=cpi_guard(c,d);if(!rc)rc=c->actual.close(c->actual.user,d);return rc?rc:cpi_guard(c,d);}
int current_preimage_init(struct current_preimage *c,const struct owned_volume_decoded *v,
 const struct nop_port *p,const struct cpi_owner *o)
{
 if(!c||!cpi_outside(c,v,sizeof(*v))||!cpi_outside(c,p,sizeof(*p))||!cpi_outside(c,o,sizeof(*o))||
 !p->now_ms||!p->wait_us||!p->authorize||!p->open||!p->transfer||!p->close||
 !o->check||!o->cycles||!o->sha256||o->cycle_hz!=32768U||!atomic_is_lock_free(&c->gate))return CPI_ARGUMENT;
 unsigned zero=0;if(!atomic_compare_exchange_strong(&c->gate,&zero,1))return CPI_BUSY;
 int rc=CPI_USED;if(c->initialized||c->fault)goto end;
 c->initialized=CPI_MAGIC;c->actual=*p;c->owner=*o;
 rc=CPI_ADMISSION;
 if(memcmp(v->spec.device_id,cpi_device,16)||memcmp(v->spec.volume_id,cpi_volume,16)||memcmp(v->digest,cpi_digest,32)||
 v->spec.generation!=1||v->spec.control_blocks[0]!=1057||v->spec.control_blocks[1]!=1058)goto fault;
 for(unsigned i=0;i<32;i++)if(v->spec.map_blocks[i]!=1025U+i)goto fault;
 c->filtered=(struct nop_port){c,cpi_now,cpi_wait,cpi_authorize,cpi_open,cpi_transfer,cpi_close};
 rc=nand_owned_phy_init(&c->phy,v,&c->filtered);if(rc)goto fault;goto end;
fault:c->fault=1;
end:atomic_store(&c->gate,0);return rc;
}
int current_preimage_capture(struct current_preimage *c,uint32_t index,uint64_t d,struct cpi_result *out)
{
 if(!c||c->initialized!=CPI_MAGIC||!cpi_outside(c,out,sizeof(*out))||index>=CPI_PAGES)return CPI_ARGUMENT;
 unsigned zero=0;if(!atomic_compare_exchange_strong(&c->gate,&zero,1))return CPI_BUSY;
 int rc=CPI_USED;if(c->fault||c->available||index!=c->next_index)goto refused;
 memset(&c->result,0,sizeof(c->result));c->result.attempted=1;c->result.index=index;c->result.row=65600U+index;
 c->result.next_index=++c->next_index;c->deadline=d;c->start=cpi_now(c);c->stage=CPI_OPEN;c->raw_phase=0;
 uint32_t prior_starts=c->phy.starts;
 rc=CPI_TIME;if(c->start<c->last_now||d<=c->start||d-c->start>CPI_PAGE_MS)goto finished;c->last_now=c->start;
 rc=cpi_guard(c,d);if(rc)goto finished;
 rc=nand_owned_phy_open(&c->phy,d);c->result.open_rc=rc;if(rc)goto finished;
 for(unsigned pass=0;pass<2;++pass){
  c->stage=pass?CPI_SECOND:CPI_FIRST;c->raw_phase=0;
  rc=nand_owned_phy_raw(&c->phy,c->result.row,pass?c->second:c->first,d);if(rc)goto finished;
  if(c->raw_phase!=4){rc=CPI_SHAPE;goto finished;}
  rc=cpi_guard(c,d);if(rc)goto finished;++c->result.reads;
 }
 c->stage=CPI_CLOSE;rc=nand_owned_phy_close(&c->phy,d);c->result.close_rc=rc;if(rc)goto finished;
 c->result.released=1;
 if(memcmp(c->first,c->second,CPI_BYTES)){rc=CPI_MISMATCH;goto finished;}c->result.matched=1;
 c->stage=CPI_HASHING;rc=cpi_guard(c,d);if(rc)goto finished;
 size_t actual=0;rc=c->owner.sha256(c->owner.user,c->first,CPI_BYTES,c->result.sha256,&actual);
 if(rc||actual!=32){rc=CPI_HASH;goto finished;}
 rc=cpi_guard(c,d);if(rc)goto finished;
 c->result.hash_valid=1;c->result.complete=1;c->available=1;
finished:
 if(rc){c->fault=1;if(!c->result.first.valid)cpi_failure(c,rc,UINT32_MAX,0,0,NULL,cpi_now(c),c->owner.cycles(c->owner.user));
  cpi_wipe(c->first,sizeof(c->first));cpi_wipe(c->result.sha256,sizeof(c->result.sha256));c->result.hash_valid=0;}
 cpi_wipe(c->second,sizeof(c->second));c->result.rc=rc;c->result.starts=c->phy.starts-prior_starts;
 c->result.stopped=c->phy.stopped;c->result.ready=c->phy.ready_known;c->result.a0=c->phy.a0;c->result.b0=c->phy.b0;
 c->result.retained=!c->result.released;uint64_t t=cpi_now(c);c->result.elapsed_ms=t>=c->start?t-c->start:0;
 *out=c->result;
refused:atomic_store(&c->gate,0);return rc;
}
int current_preimage_take(struct current_preimage *c,uint32_t index,uint8_t out[CPI_BYTES])
{
 if(!c||c->initialized!=CPI_MAGIC||!cpi_outside(c,out,CPI_BYTES))return CPI_ARGUMENT;
 unsigned zero=0;if(!atomic_compare_exchange_strong(&c->gate,&zero,1))return CPI_BUSY;
 int rc=CPI_USED;
 if(!c->fault&&c->available&&index==c->result.index&&c->result.complete&&c->result.hash_valid&&
 c->result.stopped&&c->result.ready&&c->result.released&&!c->result.retained){
  memcpy(out,c->first,CPI_BYTES);cpi_wipe(c->first,sizeof(c->first));c->available=0;rc=0;
 }
 atomic_store(&c->gate,0);return rc;
}
