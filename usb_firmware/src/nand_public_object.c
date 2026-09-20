/* Public-only append/recover state machine. No allocation, hardware or logs. */
#include "nand_public_object_internal.h"
#include <errno.h>
#include <string.h>
#define W (c->result->words)
const struct owned_page_binding npo_data_binding = {
 {0x12,0x34,0x56,0x78,0x12,0x34,0x42,0x34,0x92,0x34,0x56,0x78,0x90,0x12,0x34,0x56},
 {0xab,0xcd,0xef,0xff,0x12,0x34,0x42,0x34,0x92,0x34,0x56,0x78,0x90,0xab,0xcd,0xef},
 UINT64_C(0x0102030405060708),1,65537,UINT64_C(0x1122334455667788),OWNED_PAGE_DATA };
const struct owned_page_binding npo_commit_binding = {
 {0x12,0x34,0x56,0x78,0x12,0x34,0x42,0x34,0x92,0x34,0x56,0x78,0x90,0x12,0x34,0x56},
 {0xab,0xcd,0xef,0xff,0x12,0x34,0x42,0x34,0x92,0x34,0x56,0x78,0x90,0xab,0xcd,0xef},
 UINT64_C(0x0102030405060708),2,65538,UINT64_C(0x1122334455667788),OWNED_PAGE_COMMIT };
const uint8_t npo_object_id[16]={0xaa,0xbb,0xcc,0xdd,0x12,0x34,0x42,0x34,0x92,0x34,0,0x11,0x22,0x33,0x44,0x55};
static int fail(struct npo_core *c, enum npo_outcome outcome, int rc)
{
 if (!W[NP_PRIMARY_OUTCOME]) { W[NP_PRIMARY_OUTCOME]=outcome; W[NP_PRIMARY_RC]=(uint32_t)(rc?rc:-EIO); }
 return (int32_t)W[NP_PRIMARY_RC];
}
static int64_t now(struct npo_core *c) { return c->io->now(c->io->user); }
static int event(struct npo_core *c)
{
 if (!c->io->safe(c->io->user) || W[NP_READY_UNKNOWN]) return fail(c,NPO_STOP_ERROR,-EBUSY);
 int64_t limit=now(c)+2000; if(limit>c->deadline)limit=c->deadline;
 if(now(c)>=limit)return fail(c,NPO_DEADLINE_ERROR,-ETIMEDOUT);
 ++W[NP_EVENT_COUNT];
 int rc=c->observer->event(c->observer->user,c->result,limit);
 if(rc){W[NP_OBSERVER_RC]=(uint32_t)rc;return fail(c,NPO_OBSERVER_ERROR,rc);}
 if(now(c)>=limit)return fail(c,NPO_DEADLINE_ERROR,-ETIMEDOUT);
 return 0;
}
static unsigned length(enum npo_op op)
{
 if(op==PO_RAW)return 4356; if(op==PO_MAIN)return 4100;
 if(op==PO_DATA_LOAD||op==PO_COMMIT_LOAD)return 4099;
 if(op==PO_WREN||op==PO_WRDI)return 1;
 if(op==PO_ID||op==PO_LOAD||op==PO_DATA_EXEC||op==PO_COMMIT_EXEC)return 4;
 return 3;
}
static int step(struct npo_core *c,enum npo_op op,unsigned row,int64_t deadline,struct npo_reply *r)
{
 memset(r,0,sizeof(*r));r->stopped=true;
 if((unsigned)op>=PO_OP_COUNT||row>=64||W[NP_TRANSFERS]>=(c->restoring?NPO_MAX_STARTS:NPO_MAX_STARTS-24U))
  return fail(c,NPO_REFUSED,-EINVAL);
 if(!c->restoring){
  if(now(c)>=deadline||now(c)>=c->deadline)return fail(c,NPO_DEADLINE_ERROR,-ETIMEDOUT);
  int rc=c->observer->check(c->observer->user,c->deadline);
  if(rc){W[NP_OBSERVER_RC]=(uint32_t)rc;return fail(c,NPO_OBSERVER_ERROR,rc);}
 }
 if(now(c)>=deadline)return c->restoring?-ETIMEDOUT:fail(c,NPO_DEADLINE_ERROR,-ETIMEDOUT);
 W[NP_LAST_OPERATION]=op;W[NP_LAST_STARTED]=0;
 int rc=c->io->transfer(c->io->user,op,row,deadline,r);
 W[NP_LAST_STARTED]=r->started;W[NP_STOPPED]=r->stopped;
 if(r->started){
  ++W[NP_TRANSFERS];
  if(op==PO_UNLOCK||op==PO_LOCK){W[NP_A0_DIRTY]=1;W[NP_A0_VALID]=0;}
  if(op==PO_OFF||op==PO_ON){W[NP_B0_DIRTY]=1;W[NP_B0_VALID]=0;}
  if(op==PO_WREN)++W[NP_WREN_STARTS];if(op==PO_WRDI)++W[NP_WRDI_STARTS];
  if(op==PO_WREN||op==PO_WRDI)W[NP_C0_VALID]=0;
  if(op==PO_LOAD||op==PO_DATA_EXEC||op==PO_COMMIT_EXEC)W[NP_READY_UNKNOWN]=1;
  if(op==PO_DATA_LOAD||op==PO_COMMIT_LOAD)++W[NP_PROGRAM_LOADS];
  if(op==PO_DATA_EXEC||op==PO_COMMIT_EXEC){++W[NP_PROGRAM_EXECUTES];W[NP_ARRAY_MAY_CHANGE]=1;W[NP_C0_VALID]=0;}
 }
 if(!r->stopped){W[NP_FAULT]=1;return c->restoring?-EBUSY:fail(c,NPO_STOP_ERROR,-EBUSY);}
 if(!rc && (!r->started||r->tx_amount!=length(op)||r->rx_amount!=length(op)||!r->bytes))rc=-EIO;
 if(rc)return c->restoring?rc:fail(c,NPO_TRANSFER_ERROR,rc);
 if(op==PO_A0){W[NP_A0_VALID]=1;W[NP_A0]=r->bytes[2];}
 if(op==PO_B0){W[NP_B0_VALID]=1;W[NP_B0]=r->bytes[2];}
 if(op==PO_C0){W[NP_C0_VALID]=1;W[NP_C0]=r->bytes[2];}
 return 0;
}
static int control(struct npo_core *c,enum npo_op op,int64_t d,uint8_t mask,uint8_t want)
{
 struct npo_reply r;int rc=step(c,op,0,d,&r);if(rc)return rc;
 if(op==PO_C0 && (r.bytes[2]&0x81))W[NP_READY_UNKNOWN]=1;
 if((r.bytes[2]&mask)!=want)return c->restoring?-EPROTO:fail(c,NPO_STATE_ERROR,-EPROTO);
 return 0;
}
static int ready(struct npo_core *c,bool program,int64_t d)
{
 int64_t limit=now(c)+(program?10:5);if(limit>d)limit=d;
 for(unsigned p=0;p<(program?32U:8U);p++){
  struct npo_reply r;
  if(now(c)>=limit)return fail(c,NPO_READY_TIMEOUT,-ETIMEDOUT);
  int rc=step(c,PO_C0,0,limit,&r);if(rc)return rc;
  uint8_t v=r.bytes[2];if(!(v&0x81))W[NP_READY_UNKNOWN]=0;
  if(v&0x80)return fail(c,NPO_STATE_ERROR,-EPROTO);
  if(v&1){c->io->wait_us(c->io->user,100);continue;}
  if(v&0x0e)return fail(c,program?NPO_PROGRAM_ERROR:NPO_STATE_ERROR,-EIO);
  /* Public milestone is strict ECC0; corrected/unsupported status is not a
   * licence to mutate this fixed post-qualification state. */
  if(!program&&W[NP_B0]==0x10&&(v&0x70))return fail(c,NPO_READBACK_ERROR,-EIO);
  if(program)++W[NP_PROGRAM_COMPLETED];return 0;
 }
 return fail(c,NPO_READY_TIMEOUT,-ETIMEDOUT);
}
static int read_page(struct npo_core *c,unsigned row,bool raw,const uint8_t **bytes)
{
 struct npo_reply r;int64_t d=now(c)+1500;if(d>c->deadline)d=c->deadline;
 if(step(c,PO_LOAD,row,d,&r)||ready(c,false,d)||step(c,raw?PO_RAW:PO_MAIN,row,d,&r))return (int32_t)W[NP_PRIMARY_RC];
 const uint8_t *p=r.bytes+4;
 if(control(c,PO_C0,d,raw?0x8f:0xff,0)||control(c,PO_B0,d,0xff,raw?0:0x10))return (int32_t)W[NP_PRIMARY_RC];
 if(now(c)>=d)return fail(c,NPO_DEADLINE_ERROR,-ETIMEDOUT);
 *bytes=p;return 0;
}
static int pair_read(struct npo_core *c)
{
 const uint8_t *p;struct owned_page_view v;
 if(read_page(c,1,false,&p))return (int32_t)W[NP_PRIMARY_RC];
 if(memcmp(p,c->data,4096)||owned_page_validate(p,4096,&npo_data_binding,&c->io->hash,&v)!=OWNED_PAGE_VALID)
  return fail(c,NPO_READBACK_ERROR,-EIO);
 W[NP_DATA_VALID]=1;c->io->wipe_rx(c->io->user);
 if(read_page(c,2,false,&p))return (int32_t)W[NP_PRIMARY_RC];
 if(memcmp(p,c->commit,4096)||owned_page_match_commit(p,4096,&npo_commit_binding,npo_object_id,c->data,4096,&npo_data_binding,&c->io->hash)!=OWNED_PAGE_VALID)
  return fail(c,NPO_READBACK_ERROR,-EIO);
 W[NP_COMMIT_VALID]=1;c->io->wipe_rx(c->io->user);return 0;
}
static int program(struct npo_core *c,bool commit)
{
 struct npo_reply r;unsigned row=commit?2U:1U;
 W[NP_PHASE]=commit?NPO_COMMIT_PROGRAM:NPO_DATA_PROGRAM;
 if(control(c,PO_A0,c->deadline,0xff,0x48)||control(c,PO_B0,c->deadline,0xff,0x10)||control(c,PO_C0,c->deadline,0x8f,0)||event(c))return (int32_t)W[NP_PRIMARY_RC];
 if(step(c,PO_WREN,row,c->deadline,&r)||control(c,PO_C0,c->deadline,0x8f,2)||event(c))return (int32_t)W[NP_PRIMARY_RC];
 if(step(c,commit?PO_COMMIT_LOAD:PO_DATA_LOAD,row,c->deadline,&r)||control(c,PO_C0,c->deadline,0x8f,2)||event(c))return (int32_t)W[NP_PRIMARY_RC];
 if(step(c,commit?PO_COMMIT_EXEC:PO_DATA_EXEC,row,c->deadline,&r)||ready(c,true,c->deadline))return (int32_t)W[NP_PRIMARY_RC];
 c->io->wipe_rx(c->io->user);
 const uint8_t *p;
 if(read_page(c,row,false,&p)||memcmp(p,commit?c->commit:c->data,4096))return fail(c,NPO_READBACK_ERROR,-EIO);
 c->io->wipe_rx(c->io->user);return 0;
}
static int data_run(struct npo_core *c)
{
 struct npo_reply r;const uint8_t *p;
 W[NP_PHASE]=NPO_PREFLIGHT;
 for(unsigned i=0;i<3;i++){
  if(step(c,PO_ID,0,c->deadline,&r))return (int32_t)W[NP_PRIMARY_RC];
  if(r.bytes[2]!=0x2c||r.bytes[3]!=0x35)return fail(c,NPO_ID_ERROR,-EPROTO);
 }
 if(control(c,PO_C0,c->deadline,0xff,0)||control(c,PO_A0,c->deadline,0xff,0x7c)||control(c,PO_B0,c->deadline,0xff,0x10))return (int32_t)W[NP_PRIMARY_RC];
 W[NP_PHASE]=NPO_PATTERN;
 if(read_page(c,0,false,&p))return (int32_t)W[NP_PRIMARY_RC];
 if(memcmp(p,c->io->qualified_pattern,4096))return fail(c,NPO_PATTERN_ERROR,-EIO);
 W[NP_PATTERN_VALID]=1;c->io->wipe_rx(c->io->user);
 if(W[NP_MODE]==NPO_RECOVER_ONLY){W[NP_PHASE]=NPO_RECOVERY;return pair_read(c);}
 /* This checkpoint consumes external durable intent BEFORE any SET FEATURES. */
 W[NP_WRITE_INTENT]=1;if(event(c))return (int32_t)W[NP_PRIMARY_RC];
 if(step(c,PO_OFF,0,c->deadline,&r)||control(c,PO_B0,c->deadline,0xff,0)||control(c,PO_C0,c->deadline,0x8f,0))return (int32_t)W[NP_PRIMARY_RC];
 W[NP_PHASE]=NPO_TAIL;
 if(read_page(c,0,true,&p))return (int32_t)W[NP_PRIMARY_RC];
 if(memcmp(p,c->io->qualified_pattern,4096)||p[4096]!=0xff||p[4097]!=0xff)
  return fail(c,NPO_PATTERN_ERROR,-EIO);
 c->io->wipe_rx(c->io->user);
 for(unsigned row=1;row<64;row++){
  if(read_page(c,row,true,&p))return (int32_t)W[NP_PRIMARY_RC];
  for(unsigned i=0;i<4352;i++)if(p[i]!=0xff)return fail(c,NPO_TAIL_NOT_BLANK,-EIO);
  ++W[NP_BLANK_ROWS];c->io->wipe_rx(c->io->user);
  if((row%8)==0 && event(c))return (int32_t)W[NP_PRIMARY_RC];
 }
 if(step(c,PO_ON,0,c->deadline,&r)||control(c,PO_B0,c->deadline,0xff,0x10)||control(c,PO_C0,c->deadline,0x8f,0)||event(c))return (int32_t)W[NP_PRIMARY_RC];
 if(step(c,PO_UNLOCK,0,c->deadline,&r)||control(c,PO_A0,c->deadline,0xff,0x48))return (int32_t)W[NP_PRIMARY_RC];
 if(program(c,false)||program(c,true))return (int32_t)W[NP_PRIMARY_RC];
 W[NP_PHASE]=NPO_RECOVERY;return pair_read(c);
}
static void restore(struct npo_core *c)
{
 bool dirty=W[NP_A0_DIRTY]||W[NP_B0_DIRTY]||W[NP_WREN_STARTS];
 if(!c->io->safe(c->io->user)||W[NP_READY_UNKNOWN]){W[NP_FAULT]=1;return;}
 c->restoring=true;W[NP_PHASE]=NPO_RESTORE;int64_t d=now(c)+100;
 struct npo_reply r;int rc=control(c,PO_C0,d,0x81,0);
 if(!rc&&(W[NP_C0]&2)){
  /* Never disarm an unknown pre-existing owner or mutate in recovery mode. */
  if(!W[NP_WREN_STARTS])rc=-EPROTO;
  else {rc=step(c,PO_WRDI,0,d,&r);if(!rc)rc=control(c,PO_C0,d,0x83,0);}
 }
 if(!rc&&W[NP_A0_DIRTY]){rc=step(c,PO_LOCK,0,d,&r);if(!rc)rc=control(c,PO_A0,d,0xff,0x7c);}
 if(!rc&&W[NP_B0_DIRTY]){rc=step(c,PO_ON,0,d,&r);if(!rc)rc=control(c,PO_B0,d,0xff,0x10);}
 if(!rc)rc=control(c,PO_A0,d,0xff,0x7c);
 if(!rc)rc=control(c,PO_B0,d,0xff,0x10);
 if(!rc)rc=control(c,PO_C0,d,0x83,0);
 W[NP_RESTORE_RC]=(uint32_t)rc;
 if(!rc){W[NP_A0_DIRTY]=0;W[NP_B0_DIRTY]=0;W[NP_RESTORED]=1;}
 else if(dirty){W[NP_FAULT]=1;}
 /* Sticky P/E failure is reported, never cleared by another execute/reset. */
 if(!rc&&(W[NP_C0]&0x0c))fail(c,NPO_PROGRAM_ERROR,-EIO);
}
void npo_core_wipe(struct npo_core *c)
{
 volatile uint8_t *a=c->data,*b=c->commit,*p=c->payload;
 for(unsigned i=0;i<4096;i++){a[i]=0;b[i]=0;}for(unsigned i=0;i<2048;i++)p[i]=0;
}
int npo_core_run(struct npo_core *c,struct npo_result *result,enum npo_mode mode,
 const struct npo_io *io,const struct npo_observer *observer)
{
 if(!c||!result||!io||!observer||!observer->check||!observer->event||!io->transfer||!io->now||!io->wait_us||!io->safe||!io->wipe_rx||!io->qualified_pattern||!io->hash.sha256||(mode!=NPO_WRITE_ONCE&&mode!=NPO_RECOVER_ONLY))return -EINVAL;
 memset(c,0,sizeof(*c));memset(result,0,sizeof(*result));c->result=result;c->io=io;c->observer=observer;c->begin=now(c);c->deadline=c->begin+NPO_DATA_MS;
 W[NP_MODE]=mode;W[NP_STOPPED]=1;W[NP_BLOCK_QUARANTINED]=1;
 for(unsigned i=0;i<2048;i++)c->payload[i]=(uint8_t)(73U*i+19U*(i>>8)+0x5aU);
 if(owned_page_build_data(c->data,4096,&npo_data_binding,c->payload,2048,&io->hash)!=OWNED_PAGE_VALID||owned_page_build_commit(c->commit,4096,&npo_commit_binding,npo_object_id,c->data,4096,&npo_data_binding,&io->hash)!=OWNED_PAGE_VALID)fail(c,NPO_HASH_ERROR,-EIO);
 else (void)data_run(c);
 /* Restore only after some NAND access; a hash failure must not touch pins. */
 if(W[NP_TRANSFERS])restore(c);
 if(io->safe(io->user)){io->wipe_rx(io->user);W[NP_RAM_SCRUBBED]=1;}
 if(now(c)>=c->deadline&&!W[NP_PRIMARY_OUTCOME])fail(c,NPO_DEADLINE_ERROR,-ETIMEDOUT);
 W[NP_ELAPSED_MS]=(uint32_t)(now(c)-c->begin);
 if(W[NP_FAULT]||W[NP_RESTORE_RC]){W[NP_OUTCOME]=NPO_RESTORE_ERROR;W[NP_RC]=W[NP_RESTORE_RC]?W[NP_RESTORE_RC]:(uint32_t)-EIO;}
 else if(W[NP_PRIMARY_OUTCOME]){W[NP_OUTCOME]=W[NP_PRIMARY_OUTCOME];W[NP_RC]=W[NP_PRIMARY_RC];}
 else if(W[NP_DATA_VALID]&&W[NP_COMMIT_VALID]&&W[NP_RESTORED]){W[NP_COMMITTED]=1;W[NP_OUTCOME]=NPO_VERIFIED;W[NP_PHASE]=NPO_DONE;}
 else {W[NP_OUTCOME]=NPO_CLEANUP_ERROR;W[NP_RC]=(uint32_t)-EIO;}
 npo_core_wipe(c);return (int32_t)W[NP_RC];
}
