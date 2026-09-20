#include "recording_phy_probe.h"
#include <limits.h>
#include <string.h>
static const uint8_t prp_device[16]={0x6a,0x5f,0x3e,0xaf,0xbb,0x97,0x51,0xfb,0xa9,0x93,0xe2,0x8d,0xcf,0x2b,0x87,0x10};
static const uint8_t prp_volume[16]={0x65,0xdb,0xaf,0x95,0x2c,0x66,0x4f,0x48,0xa1,0x3f,0xd2,0x2a,0x18,0x13,0x63,0xab};
static const uint8_t prp_digest[32]={0x11,0xc7,0xcf,0x70,0x13,0xbd,0x8e,0xfe,0x7d,0x17,0x63,0xad,0xef,0xa0,0x60,0x62,0xc5,0xc8,0x37,0x29,0xf3,0x15,0x80,0x5b,0xfc,0xca,0xda,0x49,0x4a,0x87,0xaf,0x8b};
static void prp_wipe(void *p,size_t n){volatile uint8_t *b=p;while(n--)*b++=0;}
static int prp_outside(const struct phy_read_probe *c,const void *p,size_t n)
{uintptr_t a=(uintptr_t)c,b=(uintptr_t)p;return p&&b<=UINTPTR_MAX-n&&a<=UINTPTR_MAX-sizeof(*c)&&!(a<b+n&&b<a+sizeof(*c));}
static uint64_t prp_now(void *u){struct phy_read_probe *c=u;return c->actual.now_ms(c->actual.user);}
static int prp_guard(struct phy_read_probe *c,uint64_t d)
{
 uint64_t t=prp_now(c);
 if(d!=c->deadline||t<c->last_now||t>=d)return PRP_TIME;c->last_now=t;
 if(c->owner.check(c->owner.user,d))return PRP_ADMISSION;
 t=prp_now(c);if(t<c->last_now||t>=d)return PRP_TIME;c->last_now=t;return 0;
}
static void prp_failure(struct phy_read_probe *c,int rc,uint32_t op,uint32_t row,uint32_t bytes,uint32_t fast,
 const struct nop_transfer_result *r,uint64_t before,uint32_t cycle)
{
 if(c->result.first.valid)return;
 struct prp_fault *f=&c->result.first;f->valid=1;f->stage=c->stage;f->rc=rc;
 f->opcode=op;f->row=row;f->bytes=bytes;f->fast=fast;f->before_ms=before;f->after_ms=prp_now(c);
 f->elapsed_cycles=c->owner.cycles(c->owner.user)-cycle;f->cycle_hz=c->owner.cycle_hz;
 if(r){f->started=r->started;f->stopped=r->stopped;f->tx_amount=r->tx_amount;f->rx_amount=r->rx_amount;}
}
static int prp_shape(struct phy_read_probe *c,const uint8_t *tx,uint32_t n,uint32_t fast)
{
 if(tx!=c->phy.tx||fast>1)return 0;
 if(n==4&&!fast&&tx[0]==0x9f)return c->stage==PRP_OPEN&&!tx[1]&&!tx[2]&&!tx[3];
 if(n==3&&!fast&&tx[0]==0x0f)return !tx[2]&&(tx[1]==0xa0||tx[1]==0xb0||tx[1]==0xc0);
 if(n==4&&!fast&&tx[0]==0x13){uint32_t row=((uint32_t)tx[1]<<16)|((uint32_t)tx[2]<<8)|tx[3];
  return c->stage==PRP_READ&&row==c->expected_row&&(row==67652U||row==67653U)&&!c->cache_permitted;}
 if(n==4100&&fast&&tx[0]==3&&c->stage==PRP_READ&&c->cache_permitted&&c->loaded==c->expected_row){
  unsigned nonzero=0;for(unsigned i=1;i<n;i++)nonzero|=tx[i];return !nonzero;}
 return 0;
}
static int prp_transfer(void *u,const uint8_t *tx,uint8_t *rx,uint32_t n,uint32_t fast,uint64_t d,struct nop_transfer_result *r)
{
 struct phy_read_probe *c=u;memset(r,0,sizeof(*r));uint64_t before=prp_now(c);uint32_t cycle=c->owner.cycles(c->owner.user);
 uint32_t op=tx==c->phy.tx?tx[0]:UINT32_MAX;int rc=prp_guard(c,d);
 if(!rc&&rx==c->phy.rx&&prp_shape(c,tx,n,fast)){
  if(op==0x13){c->loaded=c->expected_row;c->cache_permitted=1;}
  if(op==3)c->cache_permitted=0;
  rc=c->actual.transfer(c->actual.user,tx,rx,n,fast,d,r);
  if(!rc)rc=prp_guard(c,d);
 }else if(!rc){r->stopped=c->phy.stopped;rc=PRP_SHAPE;}
 if(rc)prp_failure(c,rc,op,c->stage==PRP_READ?c->expected_row:UINT32_MAX,n,fast,r,before,cycle);
 return rc;
}
static void prp_wait(void *u,uint32_t us){struct phy_read_probe *c=u;c->actual.wait_us(c->actual.user,us);}
static int prp_authorize(void *u,uint32_t access,uint32_t row,uint64_t d)
{struct phy_read_probe *c=u;return prp_guard(c,d)||access!=NOP_READ||c->stage!=PRP_READ||row!=c->expected_row||
 (row!=67652U&&row!=67653U)?PRP_ADMISSION:c->actual.authorize(c->actual.user,access,row,d);}
static int prp_open(void *u,uint64_t d)
{struct phy_read_probe *c=u;int rc=prp_guard(c,d);if(!rc)rc=c->actual.open(c->actual.user,d);return rc?rc:prp_guard(c,d);}
static int prp_close(void *u,uint64_t d)
{struct phy_read_probe *c=u;int rc=prp_guard(c,d);if(!rc)rc=c->actual.close(c->actual.user,d);return rc?rc:prp_guard(c,d);}
int phy_read_probe_run(struct phy_read_probe *c,const struct owned_volume_decoded *v,
 const struct nop_port *p,const struct prp_owner *o,uint64_t d,struct prp_result *out)
{
 if(!c||!prp_outside(c,v,sizeof(*v))||!prp_outside(c,p,sizeof(*p))||!prp_outside(c,o,sizeof(*o))||
 !prp_outside(c,out,sizeof(*out))||!p->now_ms||!p->wait_us||!p->authorize||!p->open||!p->transfer||!p->close||
 !o->check||!o->cycles||o->cycle_hz!=32768U||!atomic_is_lock_free(&c->gate))return PRP_ARGUMENT;
 unsigned zero=0;if(!atomic_compare_exchange_strong(&c->gate,&zero,1))return PRP_BUSY;
 if(c->used){atomic_store(&c->gate,0);return PRP_USED;}c->used=1;c->result.attempted=1;
 c->actual=*p;c->owner=*o;c->deadline=d;c->start=c->last_now=prp_now(c);c->result.last_row=UINT32_MAX;
 int rc=PRP_ADMISSION;
 if(d<=c->start||d-c->start>PRP_MAX_MS||d-c->start<PRP_MIN_MS||
 memcmp(v->spec.device_id,prp_device,16)||memcmp(v->spec.volume_id,prp_volume,16)||
 memcmp(v->digest,prp_digest,32)||v->spec.generation!=1||v->spec.control_blocks[0]!=1057||v->spec.control_blocks[1]!=1058)goto finished;
 for(unsigned i=0;i<32;i++)if(v->spec.map_blocks[i]!=1025U+i)goto finished;
 c->filtered=(struct nop_port){c,prp_now,prp_wait,prp_authorize,prp_open,prp_transfer,prp_close};
 rc=nand_owned_phy_init(&c->phy,v,&c->filtered);if(rc)goto finished;
 c->stage=PRP_OPEN;rc=nand_owned_phy_open(&c->phy,d);c->result.open_rc=rc;if(rc)goto finished;
 c->stage=PRP_READ;
 uint64_t read_start=prp_now(c);
 if(read_start>=d||d-read_start<PRP_MIN_MS){rc=PRP_TIME;goto finished;}
 while(c->result.reads<PRP_MAX_READS){
  rc=prp_guard(c,d);if(rc)goto finished;
  if(c->result.reads>=2&&c->last_now-read_start>=PRP_MIN_MS)break;
  c->expected_row=67652U+(c->result.reads&1U);c->result.last_row=c->expected_row;
  rc=nand_owned_phy_read(&c->phy,c->expected_row,c->main,&c->result.ecc,d);
  prp_wipe(c->main,sizeof(c->main));if(rc)goto finished;
  if(c->result.ecc!=0){rc=PRP_ECC;goto finished;}
  rc=prp_guard(c,d);if(rc)goto finished;++c->result.reads;
 }
 if(c->result.reads<2||c->last_now-read_start<PRP_MIN_MS){rc=PRP_LIMIT;goto finished;}
 c->stage=PRP_CLOSE;rc=nand_owned_phy_close(&c->phy,d);c->result.close_rc=rc;
 if(!rc){c->result.complete=1;c->result.released=1;}
finished:
 if(rc&&!c->result.first.valid)prp_failure(c,rc,UINT32_MAX,c->result.last_row,0,0,NULL,prp_now(c),c->owner.cycles(c->owner.user));
 prp_wipe(c->main,sizeof(c->main));c->result.output_scrubbed=1;
 c->result.rc=rc;c->result.starts=c->phy.starts;c->result.stopped=c->phy.stopped;c->result.ready=c->phy.ready_known;
 c->result.retained=!c->result.released;uint64_t t=prp_now(c);c->result.elapsed_ms=t>=c->start?t-c->start:0;
 *out=c->result;atomic_store(&c->gate,0);return rc;
}
