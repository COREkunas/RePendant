/* Fixed public two-page experiment. No allocation, logging or hardware here. */
#include "nand_write_rate_internal.h"
#include "nand_public_object_internal.h"
#include <errno.h>
#include <string.h>
#define W (c->result->words)
#if defined(__GNUC__)
#define RETAIN __attribute__((used,retain))
#else
#define RETAIN
#endif
const uint32_t nand_write_rate_rates[2][2] RETAIN={{125000,0x02000000},{2000000,0x20000000}};
const uint32_t nand_write_rate_bounds[16] RETAIN=
 {1024,65539,65540,64,4096,4352,4099,1707,8,120000,1500,350000,100,8,32,5000};
const uint8_t nand_write_rate_ops[WO_COUNT][4] RETAIN={
 {0x9f,0,0,0},{0x0f,0xc0,0,0},{0x0f,0xa0,0,0},{0x0f,0xb0,0,0},
 {0x1f,0xb0,0,0},{0x1f,0xb0,0x10,0},{0x1f,0xa0,0x48,0},{0x1f,0xa0,0x7c,0},
 {0x13,1,0,0},{0x03,0,0,0},{0x03,0,0,0},{0x06,0,0,0},{0x04,0,0,0},
 {0x02,0,0,0},{0x02,0,0,0},{0x10,1,0,3},{0x10,1,0,4}};
static const uint8_t nand_write_rate_pattern_sha[2][32] RETAIN={
 {0xec,0x88,0x78,0x84,0x32,0xbf,0xd7,0x99,0xa1,0x1e,0xb8,0x93,0x6e,0xd2,0x41,0x5d,0x8e,0x1a,0x43,0xa1,0xb7,0x1f,0xe0,0x24,0x9d,0xa0,0x95,0x17,0x71,0x73,0x6b,0x49},
 {0x57,0x6a,0xe2,0xe5,0x7e,0x7d,0x17,0x93,0xf1,0x48,0xe8,0xf9,0xe7,0xe1,0x4c,0xc5,0xaf,0x51,0xcd,0xfd,0x1d,0x90,0xc8,0xa2,0x17,0x72,0x03,0xeb,0x37,0x26,0x8f,0xc7}};
static uint32_t le32(const uint8_t *p)
{return (uint32_t)p[0]|((uint32_t)p[1]<<8)|((uint32_t)p[2]<<16)|((uint32_t)p[3]<<24);}
static void put32(uint8_t *p,uint32_t v)
{for(unsigned i=0;i<4;i++)p[i]=(uint8_t)(v>>(8*i));}
static uint32_t crc32(const uint8_t *p,size_t n)
{uint32_t v=UINT32_MAX;for(size_t i=0;i<n;i++){v^=p[i];for(unsigned b=0;b<8;b++)v=(v>>1)^(0xedb88320U&(0U-(v&1)));}return ~v;}
static bool nontrivial(const uint8_t *p)
{unsigned z=0,f=0;for(unsigned i=0;i<32;i++){z|=p[i];f|=(unsigned)(p[i]^255);}return z&&f;}
void nwr_pattern(uint8_t out[4096],unsigned s)
{
 if(!out||s>1)return;
 for(unsigned i=0;i<4096;i++)out[i]=(uint8_t)(37U*i+19U*(i>>8)+0x71U+53U*s);
 memcpy(out,"OPNDWRT1",8);put32(out+8,65539U+s);put32(out+12,nand_write_rate_rates[s][0]);
}
static int fail(struct nwr_core *c,enum nwr_outcome o,int rc)
{if(!W[NW_PRIMARY_OUTCOME]){W[NW_PRIMARY_OUTCOME]=o;W[NW_PRIMARY_RC]=(uint32_t)(rc?rc:-EIO);}return (int32_t)W[NW_PRIMARY_RC];}
static int64_t now(struct nwr_core *c){return c->io->now(c->io->user);}
static uint32_t cycles(struct nwr_core *c){return c->io->cycles(c->io->user);}
static void wipe_rx(struct nwr_core *c)
{
 if(!W[NW_STOPPED])return;
 c->io->wipe_rx(c->io->user);W[NW_RAM_SCRUBBED]=1;
}
static int hash(struct nwr_core *c,const uint8_t *p,size_t n,uint8_t out[32])
{size_t written=0;int rc=c->io->hash.sha256(c->io->hash.user,p,n,out,32,&written);if(rc||written!=32){W[NW_HASH_RC]=(uint32_t)(rc?rc:-EIO);return fail(c,NWR_HASH_ERROR,rc?rc:-EIO);}return 0;}
static int check(struct nwr_core *c,int64_t limit)
{
 if(now(c)>=limit||now(c)>=c->deadline)return fail(c,NWR_DEADLINE_ERROR,-ETIMEDOUT);
 int rc=c->observer->check(c->observer->user,c->deadline);
 if(rc){W[NW_OBSERVER_RC]=(uint32_t)rc;return fail(c,NWR_OBSERVER_ERROR,rc);}
 return now(c)>=limit||now(c)>=c->deadline?fail(c,NWR_DEADLINE_ERROR,-ETIMEDOUT):0;
}
static int event(struct nwr_core *c,enum nwr_event e)
{
 if(!c->io->safe(c->io->user)||W[NW_READY_UNKNOWN])return fail(c,NWR_STOP_ERROR,-EBUSY);
 if(W[NW_EVENT_COUNT]>=NWR_MAX_EVENTS)return fail(c,NWR_REFUSED,-EOVERFLOW);
 int64_t begin=now(c),limit=begin+2000;if(limit>c->deadline)limit=c->deadline;
 if(check(c,limit))return (int32_t)W[NW_PRIMARY_RC];
 W[NW_EVENT]=e;++W[NW_EVENT_COUNT];W[NW_ELAPSED_MS]=(uint32_t)(now(c)-c->begin);
 int rc=c->observer->event(c->observer->user,c->result,limit);
 W[NW_ACK_MS]+=(uint32_t)(now(c)-begin);
 if(rc){W[NW_OBSERVER_RC]=(uint32_t)rc;return fail(c,NWR_OBSERVER_ERROR,rc);}
 return check(c,limit);
}
static unsigned length(enum nwr_op op)
{return op==WO_RAW?4356:op==WO_MAIN?4100:op==WO_LOAD0||op==WO_LOAD1?4099:op==WO_WREN||op==WO_WRDI?1:op==WO_ID||op==WO_READ_LOAD||op==WO_EXEC0||op==WO_EXEC1?4:3;}
static int step(struct nwr_core *c,enum nwr_op op,unsigned row,int64_t d,struct nwr_reply *r)
{
 memset(r,0,sizeof(*r));r->stopped=W[NW_STOPPED]!=0;r->default_valid=W[NW_DEFAULT_VALID]!=0;
 if((unsigned)op>=WO_COUNT||row>=64||W[NW_STARTS]>=(c->restoring?NWR_MAX_STARTS:NWR_MAX_STARTS-NWR_RESTORE_STARTS))
  return c->restoring?-EOVERFLOW:fail(c,NWR_REFUSED,-EOVERFLOW);
 if(!c->restoring&&check(c,d))return (int32_t)W[NW_PRIMARY_RC];
 if(now(c)>=d)return c->restoring?-ETIMEDOUT:fail(c,NWR_DEADLINE_ERROR,-ETIMEDOUT);
 W[NW_LAST_OPERATION]=op;W[NW_LAST_STARTED]=0;
 int rc=c->io->transfer(c->io->user,op,row,d,r);
 W[NW_LAST_STARTED]=r->started;W[NW_STOPPED]=r->stopped;W[NW_DEFAULT_VALID]=r->default_valid;
 W[NW_LAST_TX]=r->tx;W[NW_LAST_RX]=r->rx;W[NW_RATE_REGISTER]=r->rate_register;
 W[NW_RATE_SETS]+=r->rate_sets;W[NW_RATE_RESTORES]+=r->rate_restores;
 if(r->started){
  ++W[NW_STARTS];
  if(op==WO_RAW||op==WO_MAIN||op==WO_LOAD0||op==WO_LOAD1)W[NW_RAM_SCRUBBED]=0;
  if(op==WO_OFF||op==WO_ON){W[NW_B0_DIRTY]=1;W[NW_B0_VALID]=0;}
  if(op==WO_UNLOCK||op==WO_LOCK){W[NW_A0_DIRTY]=1;W[NW_A0_VALID]=0;}
  if(op==WO_WREN){++W[NW_WREN];W[NW_C0_VALID]=0;}
  if(op==WO_WRDI){++W[NW_WRDI];W[NW_C0_VALID]=0;}
  if(op==WO_READ_LOAD||op==WO_EXEC0||op==WO_EXEC1)W[NW_READY_UNKNOWN]=1;
  if(op==WO_LOAD0||op==WO_LOAD1){++W[NW_LOADS];W[(op==WO_LOAD0?NW_SAMPLE0:NW_SAMPLE1)+NS_LOAD_STARTED]=1;}
  if(op==WO_EXEC0||op==WO_EXEC1){++W[NW_EXECUTES];W[NW_ARRAY_MAY_CHANGE]=1;W[NW_C0_VALID]=0;W[(op==WO_EXEC0?NW_SAMPLE0:NW_SAMPLE1)+NS_EXEC_STARTED]=1;}
 }
 if(!r->stopped){W[NW_FAULT]=1;return c->restoring?-EBUSY:fail(c,NWR_STOP_ERROR,-EBUSY);}
 if(!r->default_valid){W[NW_FAULT]=1;return c->restoring?-EIO:fail(c,NWR_RATE_ERROR,-EIO);}
 if(!rc&&(!r->started||r->tx!=length(op)||r->rx!=length(op)||!r->bytes))rc=-EIO;
 if(rc)return c->restoring?rc:fail(c,NWR_TRANSFER_ERROR,rc);
 if(op==WO_C0){W[NW_C0_VALID]=1;W[NW_C0]=r->bytes[2];}
 if(op==WO_A0){W[NW_A0_VALID]=1;W[NW_A0]=r->bytes[2];}
 if(op==WO_B0){W[NW_B0_VALID]=1;W[NW_B0]=r->bytes[2];}
 return 0;
}
static int control(struct nwr_core *c,enum nwr_op op,int64_t d,unsigned mask,unsigned want)
{
 struct nwr_reply r;int rc=step(c,op,0,d,&r);if(rc)return rc;
 if(op==WO_C0&&(r.bytes[2]&0x81))W[NW_READY_UNKNOWN]=1;
 if((r.bytes[2]&mask)!=want)return c->restoring?-EPROTO:fail(c,NWR_STATE_ERROR,-EPROTO);
 return 0;
}
static int ready(struct nwr_core *c,bool program,int64_t d,unsigned sample)
{
 int64_t limit=now(c)+(program?10:5);if(limit>d)limit=d;
 for(unsigned i=0;i<(program?32U:8U);i++){
  struct nwr_reply r;if(now(c)>=limit)return fail(c,NWR_READY_TIMEOUT,-ETIMEDOUT);
  if(program)++W[(sample?NW_SAMPLE1:NW_SAMPLE0)+NS_PROGRAM_POLLS];
  if(step(c,WO_C0,0,limit,&r))return (int32_t)W[NW_PRIMARY_RC];
  uint8_t v=r.bytes[2];if(!(v&0x81))W[NW_READY_UNKNOWN]=0;
  if(v&0x80)return fail(c,NWR_STATE_ERROR,-EPROTO);
  if(v&1){c->io->wait_us(c->io->user,100);continue;}
  if(v&0x0e)return fail(c,program?NWR_PROGRAM_ERROR:NWR_STATE_ERROR,-EIO);
  if(!program&&W[NW_B0]==0x10&&(v&0x70))return fail(c,NWR_READBACK_ERROR,-EIO);
  if(program)++W[NW_COMPLETED];return 0;
 }
 return fail(c,NWR_READY_TIMEOUT,-ETIMEDOUT);
}
static int read_page(struct nwr_core *c,unsigned row,bool raw,bool b0,int64_t d,const uint8_t **p)
{
 struct nwr_reply r;W[NW_ROW_INDEX]=row;
 if(step(c,WO_READ_LOAD,row,d,&r)||ready(c,false,d,0)||step(c,raw?WO_RAW:WO_MAIN,row,d,&r))return (int32_t)W[NW_PRIMARY_RC];
 const uint8_t *bytes=r.bytes+4;
 /* HAL has separate control RX: these controls cannot overwrite cache bytes. */
 if(control(c,WO_C0,d,raw?0x8f:0xff,0)||(b0&&control(c,WO_B0,d,0xff,raw?0:0x10)))return (int32_t)W[NW_PRIMARY_RC];
 if(check(c,d))return (int32_t)W[NW_PRIMARY_RC];*p=bytes;return 0;
}
static int prepare(struct nwr_core *c)
{
 const struct nwr_binding *b=&nand_write_rate_binding;uint8_t h[32];
 if(b->enabled!=1||!nontrivial(b->table_sha)||!nontrivial(b->block_sha)||!nontrivial(b->manifest_sha)||!nontrivial(b->history_sha))return fail(c,NWR_BINDING_ERROR,-EPERM);
 if(hash(c,b->rows,sizeof(b->rows),h))return (int32_t)W[NW_PRIMARY_RC];
 if(memcmp(h,b->table_sha,32))return fail(c,NWR_BINDING_ERROR,-EIO);
 for(unsigned i=0;i<64;i++){const uint8_t *r=b->rows+44*i;if(le32(r)!=65536+i||le32(r+4)!=4352||!nontrivial(r+12))return fail(c,NWR_BINDING_ERROR,-EINVAL);}
 for(unsigned i=0;i<2048;i++)c->payload[i]=(uint8_t)(73U*i+19U*(i>>8)+0x5aU);
 if(owned_page_build_data(c->data,4096,&npo_data_binding,c->payload,2048,&c->io->hash)!=OWNED_PAGE_VALID||
    owned_page_build_commit(c->commit,4096,&npo_commit_binding,npo_object_id,c->data,4096,&npo_data_binding,&c->io->hash)!=OWNED_PAGE_VALID)return fail(c,NWR_HASH_ERROR,-EIO);
 for(unsigned s=0;s<2;s++){nwr_pattern(c->pattern,s);if(hash(c,c->pattern,4096,h))return (int32_t)W[NW_PRIMARY_RC];if(memcmp(h,nand_write_rate_pattern_sha[s],32))return fail(c,NWR_HASH_ERROR,-EIO);}
 W[NW_BINDING_VALID]=1;return 0;
}
static int whole_error(struct nwr_core *c,int rc)
{W[NW_HASH_RC]=(uint32_t)rc;return fail(c,NWR_HASH_ERROR,rc);}
static int preimage(struct nwr_core *c)
{
 int rc=c->io->whole_begin(c->io->user);c->whole_active=true;if(rc)return whole_error(c,rc);
 for(unsigned row=0;row<64;row++){
  int64_t d=now(c)+1500;if(d>c->deadline)d=c->deadline;const uint8_t *p;uint8_t h[32];
  if(read_page(c,row,true,false,d,&p))return (int32_t)W[NW_PRIMARY_RC];memcpy(c->first,p,4352);wipe_rx(c);
  if(read_page(c,row,true,false,d,&p)||control(c,WO_B0,d,0xff,0))return (int32_t)W[NW_PRIMARY_RC];
  if(memcmp(c->first,p,4352))return fail(c,NWR_PREIMAGE_ERROR,-EIO);
  const uint8_t *b=nand_write_rate_binding.rows+44*row;
  if(hash(c,p,4352,h))return (int32_t)W[NW_PRIMARY_RC];
  if(memcmp(h,b+12,32)||crc32(p,4352)!=le32(b+8))return fail(c,NWR_PREIMAGE_ERROR,-EIO);
  if(row==0){if(p[4096]!=255||p[4097]!=255)return fail(c,NWR_PREIMAGE_ERROR,-EIO);W[NW_MARKER_PRE]=0xffff;}
  if(row>=3){for(unsigned i=0;i<4352;i++)if(p[i]!=255)return fail(c,NWR_NOT_BLANK,-EIO);++W[NW_BLANK_TAIL];}
  rc=c->io->whole_update(c->io->user,p,4352);if(rc)return whole_error(c,rc);
  wipe_rx(c);volatile uint8_t *first=c->first;for(unsigned i=0;i<4352;i++)first[i]=0;
  ++W[NW_PREIMAGE_ROWS];
  if(check(c,d)||((row%8)==7&&event(c,NE_PREIMAGE)))return (int32_t)W[NW_PRIMARY_RC];
 }
 uint8_t h[32];rc=c->io->whole_finish(c->io->user,h);if(rc)return whole_error(c,rc);
 if(memcmp(h,nand_write_rate_binding.block_sha,32))return fail(c,NWR_PREIMAGE_ERROR,-EIO);
 rc=c->io->whole_abort(c->io->user);c->whole_active=false;if(rc){W[NW_FAULT]=1;return whole_error(c,rc);}
 W[NW_WHOLE_VALID]=1;return 0;
}
static int old_object(struct nwr_core *c,bool after)
{
 for(unsigned row=0;row<3;row++){
  int64_t d=now(c)+1500;if(d>c->deadline)d=c->deadline;const uint8_t *p;
  if(read_page(c,row,false,true,d,&p))return (int32_t)W[NW_PRIMARY_RC];
  const uint8_t *want=row==0?c->io->qualified_pattern:row==1?c->data:c->commit;
  if(memcmp(p,want,4096))return fail(c,NWR_OLD_OBJECT_ERROR,-EIO);
  if(row==2&&owned_page_match_commit(p,4096,&npo_commit_binding,npo_object_id,c->data,4096,&npo_data_binding,&c->io->hash)!=OWNED_PAGE_VALID)return fail(c,NWR_OLD_OBJECT_ERROR,-EIO);
  wipe_rx(c);W[after?NW_OLD_POST_VALID:NW_OLD_PRE_VALID]|=1U<<row;
  if(check(c,d))return (int32_t)W[NW_PRIMARY_RC];
 }
 return 0;
}
static int program(struct nwr_core *c,unsigned s)
{
 uint32_t *v=W+(s?NW_SAMPLE1:NW_SAMPLE0);struct nwr_reply r;uint8_t h[32];
 W[NW_PHASE]=s?NWR_PROGRAM1:NWR_PROGRAM0;W[NW_ROW_INDEX]=3+s;nwr_pattern(c->pattern,s);
 if(control(c,WO_C0,c->deadline,0x8f,0)||event(c,NE_BEFORE_WREN))return (int32_t)W[NW_PRIMARY_RC];
 if(step(c,WO_WREN,3+s,c->deadline,&r)||control(c,WO_C0,c->deadline,0x8f,2)||event(c,NE_BEFORE_LOAD))return (int32_t)W[NW_PRIMARY_RC];
 int load_rc=step(c,s?WO_LOAD1:WO_LOAD0,3+s,c->deadline,&r);
 v[NS_LOAD_TX]=r.tx;v[NS_LOAD_RX]=r.rx;v[NS_LOAD_CYCLES]=r.cycles;v[NS_RATE_REGISTER]=r.rate_register;
 if(load_rc)return load_rc;v[NS_LOAD_VALID]=1;
 if(control(c,WO_C0,c->deadline,0x8f,2)||event(c,NE_BEFORE_EXEC))return (int32_t)W[NW_PRIMARY_RC];
 c->exec_begin=cycles(c);
 if(step(c,s?WO_EXEC1:WO_EXEC0,3+s,c->deadline,&r)||ready(c,true,c->deadline,s))return (int32_t)W[NW_PRIMARY_RC];
 v[NS_EXEC_CYCLES]=cycles(c)-c->exec_begin;v[NS_EXEC_VALID]=1;wipe_rx(c);
 for(unsigned copy=0;copy<2;copy++){
  int64_t d=now(c)+1500;if(d>c->deadline)d=c->deadline;uint32_t begin=cycles(c);const uint8_t *p;
  if(read_page(c,3+s,false,true,d,&p))return (int32_t)W[NW_PRIMARY_RC];
  if(memcmp(p,c->pattern,4096))return fail(c,NWR_READBACK_ERROR,-EIO);
  if(hash(c,p,4096,h))return (int32_t)W[NW_PRIMARY_RC];
  if(memcmp(h,nand_write_rate_pattern_sha[s],32))return fail(c,NWR_READBACK_ERROR,-EIO);
  v[NS_CRC32]=crc32(p,4096);wipe_rx(c);
  if(check(c,d))return (int32_t)W[NW_PRIMARY_RC];
  v[NS_READ0_CYCLES+copy]=cycles(c)-begin;v[NS_READ0_VALID+copy]=1;
 }
 v[NS_SHA_VALID]=1;v[NS_ACTIVE_CYCLES]=v[NS_LOAD_CYCLES]+v[NS_EXEC_CYCLES]+v[NS_READ0_CYCLES]+v[NS_READ1_CYCLES];
 W[NW_VERIFIED_MASK]|=1U<<s;return event(c,NE_PROGRAM_VERIFIED);
}
static int data_run(struct nwr_core *c)
{
 struct nwr_reply r;W[NW_PHASE]=NWR_PREFLIGHT;
 for(unsigned i=0;i<3;i++){if(step(c,WO_ID,0,c->deadline,&r))return (int32_t)W[NW_PRIMARY_RC];if(r.bytes[2]!=0x2c||r.bytes[3]!=0x35)return fail(c,NWR_ID_ERROR,-EPROTO);}
 if(control(c,WO_C0,c->deadline,0xff,0)||control(c,WO_A0,c->deadline,0xff,0x7c)||control(c,WO_B0,c->deadline,0xff,0x10)||event(c,NE_PREFLIGHT)||control(c,WO_C0,c->deadline,0x8f,0)||step(c,WO_OFF,0,c->deadline,&r)||control(c,WO_B0,c->deadline,0xff,0)||control(c,WO_C0,c->deadline,0x8f,0))return (int32_t)W[NW_PRIMARY_RC];
 W[NW_PHASE]=NWR_PREIMAGE;if(preimage(c))return (int32_t)W[NW_PRIMARY_RC];
 if(control(c,WO_C0,c->deadline,0x8f,0)||step(c,WO_ON,0,c->deadline,&r)||control(c,WO_B0,c->deadline,0xff,0x10))return (int32_t)W[NW_PRIMARY_RC];
 W[NW_PHASE]=NWR_OLD_PRE;if(old_object(c,false)||event(c,NE_OLD_VALID))return (int32_t)W[NW_PRIMARY_RC];
 if(control(c,WO_C0,c->deadline,0x8f,0)||step(c,WO_UNLOCK,0,c->deadline,&r)||control(c,WO_A0,c->deadline,0xff,0x48)||program(c,0)||program(c,1))return (int32_t)W[NW_PRIMARY_RC];
 W[NW_PHASE]=NWR_OLD_POST;if(old_object(c,true))return (int32_t)W[NW_PRIMARY_RC];
 if(control(c,WO_C0,c->deadline,0x8f,0)||step(c,WO_OFF,0,c->deadline,&r)||control(c,WO_B0,c->deadline,0xff,0))return (int32_t)W[NW_PRIMARY_RC];
 W[NW_PHASE]=NWR_MARKER;const uint8_t *p;int64_t d=now(c)+1500;if(d>c->deadline)d=c->deadline;
 if(read_page(c,0,true,true,d,&p))return (int32_t)W[NW_PRIMARY_RC];
 if(p[4096]!=255||p[4097]!=255||memcmp(p,c->io->qualified_pattern,4096))return fail(c,NWR_OLD_OBJECT_ERROR,-EIO);
 W[NW_MARKER_POST]=0xffff;wipe_rx(c);if(check(c,d))return (int32_t)W[NW_PRIMARY_RC];
 return event(c,NE_PRESERVED);
}
static void restore(struct nwr_core *c)
{
 if(!c->io->safe(c->io->user)||W[NW_READY_UNKNOWN]||!W[NW_DEFAULT_VALID]){W[NW_FAULT]=1;W[NW_RESTORE_RC]=(uint32_t)-EBUSY;return;}
 c->restoring=true;W[NW_PHASE]=NWR_RESTORE;int64_t d=now(c)+100;struct nwr_reply r;
 int rc=control(c,WO_C0,d,0x81,0);
 if(!rc&&(W[NW_C0]&2)){if(!W[NW_WREN]||W[NW_WRDI])rc=-EPROTO;else {rc=step(c,WO_WRDI,0,d,&r);if(!rc)rc=control(c,WO_C0,d,0x83,0);}}
 if(!rc&&W[NW_A0_DIRTY])rc=step(c,WO_LOCK,0,d,&r);
 if(!rc)rc=control(c,WO_A0,d,0xff,0x7c);
 if(!rc&&W[NW_B0_DIRTY])rc=step(c,WO_ON,0,d,&r);
 if(!rc)rc=control(c,WO_B0,d,0xff,0x10);
 if(!rc)rc=control(c,WO_C0,d,0x83,0);
 W[NW_RESTORE_RC]=(uint32_t)rc;
 if(!rc){W[NW_A0_DIRTY]=0;W[NW_B0_DIRTY]=0;W[NW_RESTORED]=1;if(W[NW_C0]&0x0c)(void)fail(c,NWR_PROGRAM_ERROR,-EIO);}
 else if(W[NW_A0_DIRTY]||W[NW_B0_DIRTY]||W[NW_WREN])W[NW_FAULT]=1;
}
void nwr_core_wipe(struct nwr_core *c)
{
 volatile uint8_t *p=c->first;for(size_t i=0;i<sizeof(c->first);i++)p[i]=0;
 p=c->data;for(size_t i=0;i<sizeof(c->data);i++)p[i]=0;
 p=c->commit;for(size_t i=0;i<sizeof(c->commit);i++)p[i]=0;
 p=c->pattern;for(size_t i=0;i<sizeof(c->pattern);i++)p[i]=0;
 p=c->payload;for(size_t i=0;i<sizeof(c->payload);i++)p[i]=0;
}
int nwr_core_run(struct nwr_core *c,struct nand_write_rate_result *r,const struct nwr_io *io,const struct nand_write_rate_observer *o)
{
 if(!c||!r||!io||!o||!o->check||!o->event||!io->now||!io->cycles||!io->cycle_hz||io->cycle_hz>1000000000U||!io->wait_us||!io->transfer||!io->safe||!io->wipe_rx||!io->qualified_pattern||!io->hash.sha256||!io->whole_begin||!io->whole_update||!io->whole_finish||!io->whole_abort)return -EINVAL;
 memset(c,0,sizeof(*c));memset(r,0,sizeof(*r));c->result=r;c->io=io;c->observer=o;c->begin=now(c);c->deadline=c->begin+NWR_DATA_MS;
 W[NW_ATTEMPTED]=1;W[NW_QUARANTINED]=1;W[NW_STOPPED]=1;W[NW_DEFAULT_VALID]=1;W[NW_RAM_SCRUBBED]=1;W[NW_ROW_INDEX]=UINT32_MAX;W[NW_LAST_OPERATION]=UINT32_MAX;W[NW_CYCLE_HZ]=io->cycle_hz;
 for(unsigned s=0;s<2;s++){uint32_t *v=W+(s?NW_SAMPLE1:NW_SAMPLE0);v[NS_ROW]=65539+s;v[NS_RATE_HZ]=nand_write_rate_rates[s][0];}
 if(!prepare(c))(void)data_run(c);
 if(c->whole_active){int rc=io->whole_abort(io->user);c->whole_active=false;if(rc){W[NW_FAULT]=1;(void)whole_error(c,rc);}}
 if(W[NW_STARTS])restore(c);
 if(W[NW_STOPPED]){io->wipe_rx(io->user);W[NW_RAM_SCRUBBED]=1;}else W[NW_RAM_SCRUBBED]=0;
 if(now(c)>=c->deadline&&!W[NW_PRIMARY_OUTCOME])(void)fail(c,NWR_DEADLINE_ERROR,-ETIMEDOUT);
 W[NW_ELAPSED_MS]=(uint32_t)(now(c)-c->begin);
 if(W[NW_FAULT]||W[NW_RESTORE_RC]){W[NW_OUTCOME]=NWR_RESTORE_ERROR;W[NW_RC]=W[NW_RESTORE_RC]?W[NW_RESTORE_RC]:(uint32_t)-EIO;}
 else if(W[NW_PRIMARY_OUTCOME]){W[NW_OUTCOME]=W[NW_PRIMARY_OUTCOME];W[NW_RC]=W[NW_PRIMARY_RC];}
 else if(W[NW_VERIFIED_MASK]==3&&W[NW_OLD_POST_VALID]==7&&W[NW_MARKER_POST]==0xffff&&W[NW_RESTORED]){W[NW_OUTCOME]=NWR_VERIFIED;W[NW_PHASE]=NWR_DONE;}
 else {W[NW_OUTCOME]=NWR_CLEANUP_ERROR;W[NW_RC]=(uint32_t)-EIO;}
 nwr_core_wipe(c);return (int32_t)W[NW_RC];
}
#undef W
