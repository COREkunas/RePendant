/* SPDX-License-Identifier: Apache-2.0 */
#include "nand_owned_phy.h"
#include <limits.h>
#include <string.h>

#define NOP_MAGIC UINT32_C(0x4e4f5031)
static void wipe(void *p,size_t n){volatile uint8_t *b=p;while(n--)*b++=0;}
static int overlap(const void *a,size_t an,const void *b,size_t bn)
{uintptr_t x=(uintptr_t)a,y=(uintptr_t)b;return an>UINTPTR_MAX-x||bn>UINTPTR_MAX-y||(x<y+bn&&y<x+an);}
static int external(const struct nand_owned_phy *c,const void *p,size_t n)
{return p&&!overlap(c,sizeof(*c),p,n);}
static int failed_at(struct nand_owned_phy *c,uint32_t line)
{if(!c->fault_line)c->fault_line=line;c->fault=1;return NOP_FAULT;}
#define failed(c) failed_at((c),__LINE__)
static int guard(struct nand_owned_phy *c)
{uint64_t now=c->port.now_ms(c->port.user);if(now<c->last_now||now>=c->deadline)return failed(c);c->last_now=now;return 0;}
static int lock(struct nand_owned_phy *c,uint64_t deadline)
{
 if(!c||c->magic!=NOP_MAGIC)return NOP_ARGUMENT;
 unsigned expected=0;if(!atomic_compare_exchange_strong(&c->gate,&expected,1))return NOP_BUSY;
 if(c->fault){atomic_store(&c->gate,0);return NOP_FAULT;}
 uint64_t now=c->port.now_ms(c->port.user);
 if(now<c->last_now){failed(c);atomic_store(&c->gate,0);return NOP_FAULT;}
 if(deadline<=now||deadline-now>120000U){atomic_store(&c->gate,0);return NOP_ARGUMENT;}
 c->deadline=deadline;c->last_now=now;return 0;
}
static int unlock(struct nand_owned_phy *c,int rc)
{if(c->stopped){wipe(c->tx,sizeof(c->tx));wipe(c->rx,sizeof(c->rx));}atomic_store(&c->gate,0);return rc;}
static int member(const struct nand_owned_phy *c,uint32_t row)
{if(c->recording_extent==2U||c->recording_extent==3U)return row<2048U*64U;
 if(row>=1536U*64U)return 0;if(c->recording_extent==1U)return 1;
 for(unsigned i=0;i<34;++i)if(c->blocks[i]==row/64U)return 1;return 0;}
int nand_capacity_probe_block(uint32_t block)
{
 /* Excludes active1025..1056, control1057..1058, original1023..1024,
  * and saved factory-mapped1549..1569. Fixed samples span every address bit. */
 static const uint16_t blocks[]={0,1,511,512,1022,1059,1534,1535,1536,1537,1548,1570,1791,1792,2046,2047};
 for(unsigned i=0;i<sizeof(blocks)/sizeof(blocks[0]);++i)if(block==blocks[i])return 1;
 return 0;
}
static int admit(struct nand_owned_phy *c,uint32_t kind,uint32_t row)
{if(!member(c,row)||(c->recording_extent==2U&&kind!=NOP_READ&&!nand_capacity_probe_block(row/64U))||guard(c))return failed(c);int rc=c->port.authorize(c->port.user,kind,row,c->deadline);
 return guard(c)?NOP_FAULT:(rc?NOP_DENIED:0);}
/* Only local command constructors reach the transport. Commands never accept
 * opcode/column/chip selection from application or wire input. */
static int transfer(struct nand_owned_phy *c,uint32_t n,uint32_t fast)
{
 if(!c->opened||!c->stopped||c->fault||n<1||n>4356||guard(c))return failed(c);
 memset(c->rx,0,n);struct nop_transfer_result result={0};
 c->last_opcode=c->tx[0];
 int rc=c->port.transfer(c->port.user,c->tx,c->rx,n,fast,c->deadline,&result);
 if(result.started)++c->starts;
 c->stopped=result.stopped==1; /* Even no-START refusal may observe lost ownership. */
 if(rc||result.started!=1||result.stopped!=1||result.tx_amount!=n||result.rx_amount!=n||guard(c))return failed(c);
 return 0;
}
static int command(struct nand_owned_phy *c,uint8_t opcode)
{c->tx[0]=opcode;return transfer(c,1,0);}
static int feature(struct nand_owned_phy *c,uint8_t reg,uint32_t *value)
{c->tx[0]=0x0f;c->tx[1]=reg;c->tx[2]=0;if(transfer(c,3,0))return NOP_FAULT;*value=c->rx[2];return 0;}
static int status(struct nand_owned_phy *c)
{return feature(c,0xc0,&c->status);}
static int setting(struct nand_owned_phy *c,uint8_t reg,uint8_t value)
{
 if(!c->ready_known||(reg!=0xa0&&reg!=0xb0)||(reg==0xa0&&value!=0x48&&value!=0x7c&&!(value==0&&(c->recording_extent==2U||c->recording_extent==3U)))||
    (reg==0xb0&&value!=0&&value!=0x10))return failed(c);
 c->tx[0]=0x1f;c->tx[1]=reg;c->tx[2]=value;if(transfer(c,3,0))return NOP_FAULT;
 uint32_t *result=reg==0xa0?&c->a0:&c->b0;
 if(feature(c,reg,result)||*result!=value||status(c)||(c->status&0x8fU)!=c->media_flags)return failed(c);
 return 0;
}
static int ready(struct nand_owned_phy *c,uint32_t max_ms)
{
 uint64_t now=c->port.now_ms(c->port.user),until=now>UINT64_MAX-max_ms?UINT64_MAX:now+max_ms;
 if(until>c->deadline)until=c->deadline;
 c->ready_known=0;
 for(unsigned i=0;i<256;++i){
  /* Date the sample conservatively BEFORE GET FEATURE. DMA may capture OIP=1
   * and the codec then preempt us through END/STOP cleanup while NAND becomes
   * ready. A return time past 'until' cannot make that old sample a timeout.
   * Only a GET begun at/after the local limit can establish still-busy there.
   * The absolute job deadline and 256-poll bound are never renewed. */
  if(guard(c))return NOP_FAULT;
  uint64_t sample_begin=c->last_now;
  if(status(c))return NOP_FAULT;
  if(c->status&0x80U)return failed(c); /* No cache-read-busy command exists here. */
  if(!(c->status&1U)){c->ready_known=1;return 0;}
  if(sample_begin>=until)return failed(c);
  c->port.wait_us(c->port.user,100);if(guard(c))return NOP_FAULT;
 }
 return failed(c);
}
static int row_command(struct nand_owned_phy *c,uint8_t opcode,uint32_t row)
{c->last_row=row;c->ready_known=0;c->tx[0]=opcode;c->tx[1]=(uint8_t)(row>>16);c->tx[2]=(uint8_t)(row>>8);c->tx[3]=(uint8_t)row;return transfer(c,4,0);}
static int baseline(struct nand_owned_phy *c)
{if(!c->opened||!c->ready_known||status(c)||(c->status&0x8fU)!=c->media_flags||feature(c,0xa0,&c->a0)||c->a0!=0x7c||
 feature(c,0xb0,&c->b0)||c->b0!=0x10)return failed(c);
 return 0;}

int nand_owned_phy_init(struct nand_owned_phy *c,const struct owned_volume_decoded *v,const struct nop_port *p)
{
 if(!c||!v||!p||c->magic||!p->now_ms||!p->wait_us||!p->authorize||!p->open||!p->transfer||!p->close||
    !external(c,v,sizeof(*v))||!external(c,p,sizeof(*p))||!atomic_is_lock_free(&c->gate))return NOP_ARGUMENT;
 uint32_t blocks[34];memcpy(blocks,v->spec.map_blocks,32*sizeof(uint32_t));memcpy(blocks+32,v->spec.control_blocks,2*sizeof(uint32_t));
 for(unsigned i=0;i<34;++i){if(blocks[i]>=1536)return NOP_ARGUMENT;for(unsigned j=0;j<i;++j)if(blocks[i]==blocks[j])return NOP_ARGUMENT;}
 c->port=*p;memcpy(c->blocks,blocks,sizeof(blocks));c->recording_extent=0;c->stopped=1;c->magic=NOP_MAGIC;return 0;
}
int nand_owned_phy_init_recording_extent(struct nand_owned_phy *c,const struct nop_port *p)
{
 if(!c||!p||c->magic||!p->now_ms||!p->wait_us||!p->authorize||!p->open||!p->transfer||!p->close||
 !external(c,p,sizeof(*p))||!atomic_is_lock_free(&c->gate))return NOP_ARGUMENT;
 c->port=*p;c->recording_extent=1;c->stopped=1;c->magic=NOP_MAGIC;return 0;
}
int nand_owned_phy_init_capacity_probe(struct nand_owned_phy *c,const struct nop_port *p)
{
 int rc=nand_owned_phy_init_recording_extent(c,p);
 if(!rc)c->recording_extent=2;
 return rc;
}
int nand_owned_phy_init_full_recording(struct nand_owned_phy *c,const struct nop_port *p)
{
 int rc=nand_owned_phy_init_recording_extent(c,p);
 if(!rc)c->recording_extent=3;
 return rc;
}
int nand_owned_phy_open(struct nand_owned_phy *c,uint64_t deadline)
{
 int rc=lock(c,deadline);if(rc)return rc;if(c->opened)return unlock(c,NOP_ARGUMENT);
 /* Opening bus alone does not grant reads or mutations. */
 rc=c->port.open(c->port.user,deadline);if(rc||guard(c))return unlock(c,failed(c));c->opened=1;
 c->tx[0]=0x9f;c->tx[1]=c->tx[2]=c->tx[3]=0;
 if(transfer(c,4,0)||c->rx[2]!=0x2c||c->rx[3]!=0x35||ready(c,100))return unlock(c,failed(c));
 /* Read-only boot reconciliation observes historical P/E flags, never clears
  * them with RESET or a sacrificial array operation. WEL can safely be disabled
  * only after independently checked ready. The authority provider must reconcile
  * pending control intents before granting any subsequent mutation. */
 c->media_flags=c->status&12U;
 if((c->status&2U)&&(command(c,0x04)||status(c)))return unlock(c,failed(c));
 if(baseline(c))return unlock(c,failed(c));
 return unlock(c,0);
}
static int read_page(struct nand_owned_phy *c,uint32_t row,uint32_t column,uint32_t bytes,uint8_t *out,uint32_t *ecc,int raw,uint64_t deadline)
{
 if(!c||!bytes||(raw?(column!=0||bytes!=4352):(column>=4096||bytes>4096-column))||
    !external(c,out,bytes)||(!raw&&(!external(c,ecc,sizeof(*ecc))||overlap(out,bytes,ecc,sizeof(*ecc)))))return NOP_ARGUMENT;
 int rc=lock(c,deadline);if(rc)return rc;
 rc=admit(c,NOP_READ,row);if(rc)return unlock(c,rc);
 if(baseline(c)||(raw&&setting(c,0xb0,0))||row_command(c,0x13,row)||ready(c,5)||(c->status&0x8fU)!=c->media_flags)return unlock(c,failed(c));
 uint32_t observed=(c->status>>4U)&7U,n=bytes+4U;
 /* M70A Rev I p21: 03h,16-bit column,8 dummy clocks,requested bytes. */
 memset(c->tx,0,n);c->tx[0]=0x03;c->tx[1]=(uint8_t)(column>>8);c->tx[2]=(uint8_t)column;
 if(transfer(c,n,1))return unlock(c,NOP_FAULT);
 /* Preserve fetched bytes before tiny GET/SET responses overwrite rx. Caller
  * output is copied only after all checked restore operations: use tx as an
  * independent CPU scratch now that both DMA spans have proven STOP. */
 memcpy(c->tx+4,c->rx+4,n-4);
 if(raw){
  /* Small commands touch only tx/rx[0..2], preserving tx[4..]. */
  if(setting(c,0xb0,0x10))return unlock(c,NOP_FAULT);
 }
 if(baseline(c)||guard(c))return unlock(c,NOP_FAULT);
 memcpy(out,c->tx+4,n-4);if(!raw)*ecc=observed;return unlock(c,0);
}
int nand_owned_phy_read(struct nand_owned_phy *c,uint32_t row,uint8_t out[4096],uint32_t *ecc,uint64_t deadline)
{return read_page(c,row,0,4096,out,ecc,0,deadline);}
int nand_owned_phy_read_range(struct nand_owned_phy *c,uint32_t row,uint32_t column,uint32_t bytes,uint8_t *out,uint32_t *ecc,uint64_t deadline)
{return read_page(c,row,column,bytes,out,ecc,0,deadline);}
int nand_owned_phy_raw(struct nand_owned_phy *c,uint32_t row,uint8_t out[4352],uint64_t deadline)
{return read_page(c,row,0,4352,out,NULL,1,deadline);}
static int verify_program(struct nand_owned_phy *c,uint32_t row,const uint8_t *main)
{
 int rc=admit(c,NOP_READ,row);if(rc)return failed(c);
 if(row_command(c,0x13,row)||ready(c,5)||(c->status&0x8fU)!=c->media_flags)return failed(c);
 uint32_t ecc=(c->status>>4U)&7U;
 if(ecc!=0&&ecc!=1&&ecc!=3&&ecc!=5)return failed(c);
 memset(c->tx,0,4100);c->tx[0]=0x03;
 if(transfer(c,4100,1))return NOP_FAULT;
 for(unsigned i=0;i<4096;++i)if(c->rx[i+4]!=main[i]){c->verify_mismatch=i+1;return failed(c);}
 if(baseline(c)||guard(c))return NOP_FAULT;
 ++c->verified_programs;return 0;
}
static int verify_erase(struct nand_owned_phy *c,uint32_t row)
{
 /* Full raw readback includes spare/ECC bytes; corrected FF is insufficient.
  * The block was admitted by the native bad-marker scan before ERASE. */
 for(uint32_t p=0;p<64;++p){
  if(admit(c,NOP_READ,row+p)||setting(c,0xb0,0)||row_command(c,0x13,row+p)||ready(c,5)||
     (c->status&0x8fU)!=c->media_flags)return failed(c);
  memset(c->tx,0,4356);c->tx[0]=0x03;
  if(transfer(c,4356,1))return NOP_FAULT;
  uint32_t mismatch=0;
  for(unsigned i=0;i<4352;++i)if(c->rx[i+4]!=255){mismatch=p*4352U+i+1U;break;}
  /* Known-ready, proven-STOP content mismatch can restore ECC before fencing.
   * Transport uncertainty above must never attempt this cleanup. */
  if(setting(c,0xb0,0x10)||baseline(c)||guard(c))return NOP_FAULT;
  if(mismatch){c->verify_mismatch=mismatch;return failed(c);}
 }
 return 0;
}
static int mutation(struct nand_owned_phy *c,uint32_t row,const uint8_t *main,int erase,int checked,uint64_t deadline)
{
 if(!c||(!erase&&!external(c,main,4096))||(erase&&row%64U))return NOP_ARGUMENT;
 int rc=lock(c,deadline);if(rc)return rc;uint32_t kind=erase?NOP_ERASE:NOP_PROGRAM;
 rc=admit(c,kind,row);if(rc)return unlock(c,rc);
 /* M70A Rev I p42: A0=00 unlocks all blocks, VOLATILE only. Allowed solely
  * for the fixed diagnostic sample or explicit full recording constructor;
  * re-lock after EACH mutation. Legacy/profile1 bounds remain unchanged. */
 uint8_t protection=(c->recording_extent==2U||c->recording_extent==3U)&&row>=1536U*64U?0:0x48;
 if(baseline(c)||setting(c,0xa0,protection))return unlock(c,NOP_FAULT);
 rc=admit(c,kind,row);if(rc)return unlock(c,failed(c));
 if(command(c,0x06)||status(c)||(c->status&0x8fU)!=(c->media_flags|2U))return unlock(c,failed(c));
 if(!erase){c->tx[0]=0x02;c->tx[1]=c->tx[2]=0;memcpy(c->tx+3,main,4096);if(transfer(c,4099,1))return unlock(c,NOP_FAULT);}
 rc=admit(c,kind,row);if(rc)return unlock(c,failed(c));
 if(row_command(c,erase?0xd8:0x10,row)||ready(c,erase?100U:10U))return unlock(c,NOP_FAULT);
 uint32_t outcome=c->status&0x8fU,bit=erase?4U:8U,other=erase?8U:4U;
 if((outcome&0x81U)||(outcome&other)!=(c->media_flags&other))return unlock(c,failed(c));
 uint32_t media=outcome&bit;c->media_flags=outcome&12U;
 /* M70A Rev I p48: only successful P/E guarantees WEL clear; each operation
  * clears its own failure flag, not the other operation's flag. A diagnosed
  * failure with WEL set gets WRDI while known ready, never RESET or retry. */
 if(outcome&2U){
  if(!media){++c->wel_observed;if(!checked)return unlock(c,failed(c));}
  if(command(c,0x04)||status(c)||(c->status&0x8fU)!=c->media_flags)return unlock(c,failed(c));
 }
 if(setting(c,0xa0,0x7c)||baseline(c)||guard(c))return unlock(c,NOP_FAULT);
 if(checked&&!erase&&!media&&verify_program(c,row,main))return unlock(c,NOP_FAULT);
 if(checked&&erase&&!media&&verify_erase(c,row))return unlock(c,NOP_FAULT);
 return unlock(c,media?NOP_MEDIA:0);
}
int nand_owned_phy_program(struct nand_owned_phy *c,uint32_t row,const uint8_t in[4096],uint64_t deadline)
{return mutation(c,row,in,0,0,deadline);}
int nand_owned_phy_program_checked(struct nand_owned_phy *c,uint32_t row,const uint8_t in[4096],uint64_t deadline)
{return mutation(c,row,in,0,1,deadline);}
int nand_owned_phy_erase(struct nand_owned_phy *c,uint32_t block,uint64_t deadline)
{if(!c||block>=((c->recording_extent==2U||c->recording_extent==3U)?2048U:1536U))return NOP_ARGUMENT;return mutation(c,block*64U,NULL,1,0,deadline);}
int nand_owned_phy_erase_checked(struct nand_owned_phy *c,uint32_t block,uint64_t deadline)
{if(!c||block>=((c->recording_extent==2U||c->recording_extent==3U)?2048U:1536U))return NOP_ARGUMENT;return mutation(c,block*64U,NULL,1,1,deadline);}
int nand_owned_phy_close(struct nand_owned_phy *c,uint64_t deadline)
{
 int rc=lock(c,deadline);if(rc)return rc;
 if(baseline(c)||c->port.close(c->port.user,deadline)||guard(c))return unlock(c,failed(c));
 c->opened=0;return unlock(c,0);
}
