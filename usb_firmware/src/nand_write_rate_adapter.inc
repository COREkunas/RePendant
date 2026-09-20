/* Private future include in nand_id.c; currently NOT linked. Shared owner
 * supplies probe/setup/cleanup, permanent DMA and STOP proof. No public SPI. */
#include "nand_write_rate_internal.h"
static atomic_t nwr_entry;
static struct nwr_core nwr_workspace;
static struct nand_write_rate_result *nwr_active_result;
static const struct nand_write_rate_observer *nwr_active_observer;
static psa_hash_operation_t nwr_whole_hash=PSA_HASH_OPERATION_INIT;
static __aligned(4) uint8_t nand_write_rate_program_tx[4099];
static int probe(struct nand_id_result *,struct nand_feature_result *,
 struct nand_page0_result *,struct nand_marker_result *,
 struct nand_block1024_context *,struct nand_qualify_context *,bool);
BUILD_ASSERT(sizeof(nand_write_rate_program_tx)==4099U &&
 sizeof(nand_write_rate_program_tx)<=(SPIM_TXD_MAXCNT_MAXCNT_Msk>>SPIM_TXD_MAXCNT_MAXCNT_Pos),"Fixed program DMA MAXCNT");
BUILD_ASSERT(NRF_SPIM_FREQ_125K==0x02000000U&&NRF_SPIM_FREQ_2M==0x20000000U,"Fixed reviewed rates");
BUILD_ASSERT(NWR_WORDS==96U&&NW_SAMPLE1+20U==NWR_WORDS&&NWR_MAX_STARTS==1707U,"Fixed write-rate bounds");
static bool nwr_hw_safe(void *u)
{(void)u;return atomic_get(&stopped)&&!atomic_get(&fault)&&atomic_get(&cs_high)&&observe_aux();}
static int64_t nwr_hw_now(void *u){(void)u;return k_uptime_get();}
static uint32_t nwr_hw_cycles(void *u){(void)u;return k_cycle_get_32();}
static void nwr_hw_wait(void *u,unsigned us){(void)u;k_busy_wait(us);}
static void nwr_hw_wipe(void *u)
{
 (void)u;if(!atomic_get(&stopped))return;
 block1024_wipe();volatile uint8_t *p=nand_write_rate_program_tx;
 for(size_t i=0;i<sizeof(nand_write_rate_program_tx);i++)p[i]=0;
}
static int nwr_hw_hash(void *u,const uint8_t *p,size_t n,uint8_t *out,size_t cap,size_t *written)
{
 (void)u;*written=0;psa_hash_operation_t h=PSA_HASH_OPERATION_INIT;
 psa_status_t rc=psa_hash_setup(&h,PSA_ALG_SHA_256);
 if(rc==PSA_SUCCESS)rc=psa_hash_update(&h,p,n);
 if(rc==PSA_SUCCESS)rc=psa_hash_finish(&h,out,cap,written);
 psa_status_t aborted=psa_hash_abort(&h);if(aborted!=PSA_SUCCESS)atomic_set(&fault,1);
 return rc==PSA_SUCCESS&&aborted==PSA_SUCCESS&&*written==32?0:-EIO;
}
static int nwr_whole_begin(void *u){(void)u;return psa_hash_setup(&nwr_whole_hash,PSA_ALG_SHA_256)==PSA_SUCCESS?0:-EIO;}
static int nwr_whole_update(void *u,const uint8_t *p,size_t n){(void)u;return psa_hash_update(&nwr_whole_hash,p,n)==PSA_SUCCESS?0:-EIO;}
static int nwr_whole_finish(void *u,uint8_t out[32]){(void)u;size_t n=0;return psa_hash_finish(&nwr_whole_hash,out,32,&n)==PSA_SUCCESS&&n==32?0:-EIO;}
static int nwr_whole_abort(void *u){(void)u;if(psa_hash_abort(&nwr_whole_hash)==PSA_SUCCESS)return 0;atomic_set(&fault,1);return -EIO;}
static bool nwr_hw_allowed(enum nwr_op op,unsigned row)
{
 if(!nwr_active_result||(unsigned)op>=WO_COUNT||row>=64)return false;
 const uint32_t *w=nwr_active_result->words;
 if(w[NW_STARTS]>=(nwr_workspace.restoring?NWR_MAX_STARTS:NWR_MAX_STARTS-NWR_RESTORE_STARTS))return false;
 if(nwr_workspace.restoring){
  if(!nwr_hw_safe(NULL)||w[NW_READY_UNKNOWN])return false;
  return op==WO_C0||op==WO_A0||op==WO_B0||(op==WO_LOCK&&w[NW_A0_DIRTY])||
   (op==WO_ON&&w[NW_B0_DIRTY])||(op==WO_WRDI&&w[NW_WREN]&&!w[NW_WRDI]&&w[NW_C0_VALID]&&(w[NW_C0]&2));
 }
 if(!w[NW_BINDING_VALID]||nand_write_rate_binding.enabled!=1)return false;
 if(op==WO_ID||op==WO_C0||op==WO_A0||op==WO_B0)return true;
 if(op==WO_READ_LOAD||op==WO_RAW||op==WO_MAIN){
  bool phase=(w[NW_PHASE]==NWR_PREIMAGE&&row==w[NW_PREIMAGE_ROWS])||
   ((w[NW_PHASE]==NWR_OLD_PRE||w[NW_PHASE]==NWR_OLD_POST)&&row<=2)||
   (w[NW_PHASE]==NWR_PROGRAM0&&row==3)||(w[NW_PHASE]==NWR_PROGRAM1&&row==4)||
   (w[NW_PHASE]==NWR_MARKER&&row==0);
  if(!phase||w[NW_READY_UNKNOWN])return false;
  if(op==WO_READ_LOAD)return true;
  return w[NW_B0_VALID]&&((op==WO_RAW&&w[NW_B0]==0&&(w[NW_PHASE]==NWR_PREIMAGE||w[NW_PHASE]==NWR_MARKER))||
   (op==WO_MAIN&&w[NW_B0]==0x10&&w[NW_PHASE]!=NWR_PREIMAGE&&w[NW_PHASE]!=NWR_MARKER));
 }
 if(op==WO_OFF)return w[NW_B0_VALID]&&w[NW_B0]==0x10&&
  (w[NW_PHASE]==NWR_PREFLIGHT||(w[NW_PHASE]==NWR_OLD_POST&&w[NW_OLD_POST_VALID]==7));
 if(!w[NW_WHOLE_VALID]||w[NW_PREIMAGE_ROWS]!=64||w[NW_BLANK_TAIL]!=61)return false;
 if(op==WO_ON)return w[NW_PHASE]==NWR_PREIMAGE&&w[NW_B0_VALID]&&w[NW_B0]==0;
 if(w[NW_OLD_PRE_VALID]!=7||!w[NW_A0_VALID]||!w[NW_B0_VALID]||w[NW_B0]!=0x10)return false;
 if(op==WO_UNLOCK)return w[NW_PHASE]==NWR_OLD_PRE&&w[NW_A0]==0x7c;
 if(w[NW_A0]!=0x48||!w[NW_C0_VALID])return false;
 if(op==WO_WREN)return (w[NW_C0]&0x8f)==0&&w[NW_WREN]<2&&row==3+w[NW_WREN]&&
  w[NW_PHASE]==(uint32_t)(w[NW_WREN]?NWR_PROGRAM1:NWR_PROGRAM0)&&(!w[NW_WREN]||w[NW_VERIFIED_MASK]==1);
 if((w[NW_C0]&0x8f)!=2)return false;
 if(op==WO_LOAD0)return row==3&&w[NW_WREN]==1&&!w[NW_LOADS]&&w[NW_PHASE]==NWR_PROGRAM0;
 if(op==WO_LOAD1)return row==4&&w[NW_WREN]==2&&w[NW_LOADS]==1&&w[NW_VERIFIED_MASK]==1&&w[NW_PHASE]==NWR_PROGRAM1;
 if(op==WO_EXEC0)return row==3&&w[NW_LOADS]==1&&!w[NW_EXECUTES]&&w[NW_PHASE]==NWR_PROGRAM0;
 if(op==WO_EXEC1)return row==4&&w[NW_LOADS]==2&&w[NW_EXECUTES]==1&&w[NW_VERIFIED_MASK]==1&&w[NW_PHASE]==NWR_PROGRAM1;
 return false;
}
static int nwr_set_rate(uint32_t v)
{
 if((v!=NRF_SPIM_FREQ_125K&&v!=NRF_SPIM_FREQ_2M)||!nwr_hw_safe(NULL)||!nrf_spim_enable_check(nand_id_spim))return -EPERM;
 nrf_spim_disable(nand_id_spim);if(nand_id_spim->ENABLE!=0)goto bad;
 erratum135(false);nrf_spim_frequency_set(nand_id_spim,(nrf_spim_frequency_t)v);__DMB();
 if(nand_id_spim->FREQUENCY!=v)goto bad;
 erratum135(true);nrf_spim_enable(nand_id_spim);__DMB();
 if(!nrf_spim_enable_check(nand_id_spim))goto bad;return 0;
bad:atomic_set(&fault,1);return -EIO;
}
static int nwr_return_rate(struct nwr_reply *r)
{
 r->default_valid=false;
 if(!atomic_get(&stopped)||!atomic_get(&cs_high))return -EBUSY;
 if(nand_id_spim->FREQUENCY!=NRF_SPIM_FREQ_125K){int rc=nwr_set_rate(NRF_SPIM_FREQ_125K);if(rc)return rc;++r->rate_restores;}
 r->default_valid=nand_id_spim->FREQUENCY==NRF_SPIM_FREQ_125K;
 return r->default_valid?0:-EIO;
}
static int nwr_hw_transfer(void *u,enum nwr_op op,unsigned row,int64_t d,struct nwr_reply *r)
{
 (void)u;memset(r,0,sizeof(*r));r->stopped=atomic_get(&stopped)!=0;r->default_valid=nand_id_spim->FREQUENCY==NRF_SPIM_FREQ_125K;
 if(!nwr_hw_allowed(op,row)||!nwr_hw_safe(NULL)||!r->default_valid)return -EPERM;
 bool cache=op==WO_RAW||op==WO_MAIN,program=op==WO_LOAD0||op==WO_LOAD1;
 bool long_dma=cache||program;
 size_t n=op==WO_RAW?4356:op==WO_MAIN?4100:program?4099:op==WO_WREN||op==WO_WRDI?1:op==WO_ID||op==WO_READ_LOAD||op==WO_EXEC0||op==WO_EXEC1?4:3;
 uint8_t *tx=program?nand_write_rate_program_tx:cache?nand_marker_tx:nand_id_tx;
 uint8_t *rx=long_dma?nand_marker_rx_first:nand_id_rx;
 if(k_uptime_get()>=d)return -ETIMEDOUT;
 if(program){memcpy(tx,nand_write_rate_ops[op],3);memcpy(tx+3,nwr_workspace.pattern,4096);}
 else if(!cache){memcpy(tx,nand_write_rate_ops[op],4);if(op==WO_READ_LOAD)tx[3]=(uint8_t)row;}
 memset(rx,0,n);nrf_spim_event_clear(nand_id_spim,NRF_SPIM_EVENT_END);nrf_spim_event_clear(nand_id_spim,NRF_SPIM_EVENT_STOPPED);
 nrf_spim_tx_buffer_set(nand_id_spim,tx,n);nrf_spim_rx_buffer_set(nand_id_spim,rx,n);
 if(!all_cs_high())return -EIO;
 uint32_t rate=(op==WO_MAIN||op==WO_LOAD1)?NRF_SPIM_FREQ_2M:NRF_SPIM_FREQ_125K;
 if(rate!=NRF_SPIM_FREQ_125K){int rc=nwr_set_rate(rate);if(rc){r->default_valid=false;return rc;}++r->rate_sets;}
 r->rate_register=nand_id_spim->FREQUENCY;r->default_valid=rate==NRF_SPIM_FREQ_125K;
 if(!nwr_workspace.restoring){
  int rc=nwr_active_observer->check(nwr_active_observer->user,nwr_workspace.deadline);
  if(rc){nwr_active_result->words[NW_OBSERVER_RC]=(uint32_t)rc;
   if(!nwr_active_result->words[NW_PRIMARY_OUTCOME]){nwr_active_result->words[NW_PRIMARY_OUTCOME]=NWR_OBSERVER_ERROR;nwr_active_result->words[NW_PRIMARY_RC]=(uint32_t)rc;}
   (void)nwr_return_rate(r);return rc;}
 }
 if(k_uptime_get()>=d){(void)nwr_return_rate(r);return -ETIMEDOUT;}
 nrf_gpio_pin_clear(nand_id_cs_pins[1]);atomic_clear(&cs_high);k_busy_wait(2);
 if(k_uptime_get()>=d){(void)all_cs_high();(void)nwr_return_rate(r);return -ETIMEDOUT;}
 atomic_clear(&stopped);if(long_dma)atomic_clear(&marker_ram_scrubbed);
 uint32_t begin=k_cycle_get_32();__DMB();nrf_spim_task_trigger(nand_id_spim,NRF_SPIM_TASK_START);r->started=true;r->stopped=false;
 bool ended=long_dma?wait_cache_end(d):wait_event(NRF_SPIM_EVENT_END,d);
 r->cycles=(uint32_t)(k_cycle_get_32()-begin);bool high=all_cs_high();
 nrf_spim_event_clear(nand_id_spim,NRF_SPIM_EVENT_STOPPED);nrf_spim_task_trigger(nand_id_spim,NRF_SPIM_TASK_STOP);
 if(!wait_event(NRF_SPIM_EVENT_STOPPED,k_uptime_get()+3)){atomic_set(&fault,1);return -EBUSY;}
 __DMB();atomic_set(&stopped,1);r->stopped=true;r->tx=nrf_spim_tx_amount_get(nand_id_spim);r->rx=nrf_spim_rx_amount_get(nand_id_spim);
 int rate_rc=nwr_return_rate(r);if(rate_rc)return rate_rc;
 if(!ended||!high||r->tx!=n||r->rx!=n)return -EIO;r->bytes=rx;return 0;
}
static int nwr_adapter_run(void)
{
 const struct nwr_io io={NULL,nwr_hw_now,nwr_hw_cycles,sys_clock_hw_cycles_per_sec(),nwr_hw_wait,
  nwr_hw_transfer,nwr_hw_safe,nwr_hw_wipe,nand_qualify_program_tx+3,{NULL,nwr_hw_hash},
  nwr_whole_begin,nwr_whole_update,nwr_whole_finish,nwr_whole_abort};
 int rc=nwr_core_run(&nwr_workspace,nwr_active_result,&io,nwr_active_observer);
 if(nwr_active_result->words[NW_FAULT]||nwr_active_result->words[NW_READY_UNKNOWN]||nwr_active_result->words[NW_A0_DIRTY]||nwr_active_result->words[NW_B0_DIRTY])atomic_set(&fault,1);
 nwr_workspace.result=NULL;nwr_workspace.io=NULL;nwr_workspace.observer=NULL;return rc;
}
static void nwr_cleanup_check(void)
{
 if(!nwr_active_result)return;
 uint32_t *w=nwr_active_result->words;
 if(!atomic_get(&stopped)){w[NW_DEFAULT_VALID]=0;return;}
 w[NW_DEFAULT_VALID]=nand_id_spim->FREQUENCY==NRF_SPIM_FREQ_125K;
 if(w[NW_STARTS]&&!w[NW_DEFAULT_VALID])atomic_set(&fault,1);
}
int nand_write_rate_run(struct nand_write_rate_result *r,const struct nand_write_rate_observer *o)
{
 if(!r||!o||!o->check||!o->event)return -EINVAL;
 if(!atomic_cas(&nwr_entry,0,1))return -EBUSY;
 memset(r,0,sizeof(*r));r->words[NW_QUARANTINED]=1;r->words[NW_ROW_INDEX]=UINT32_MAX;
 r->words[NW_LAST_OPERATION]=UINT32_MAX;
 for(unsigned i=0;i<2;i++){uint32_t *v=r->words+(i?NW_SAMPLE1:NW_SAMPLE0);v[NS_ROW]=65539U+i;v[NS_RATE_HZ]=i?2000000U:125000U;}
 int rc;
 if(nand_write_rate_binding.enabled!=1){rc=-EPERM;r->words[NW_OUTCOME]=NWR_BINDING_ERROR;goto done;}
 nwr_active_result=r;nwr_active_observer=o;
 nwr_whole_hash=(psa_hash_operation_t)PSA_HASH_OPERATION_INIT;
 struct nand_id_result common;rc=probe(&common,NULL,NULL,NULL,NULL,NULL,false);
 struct nand_id_state s;nand_id_get_state(&s);
 r->words[NW_STOPPED]=s.clock_stopped;r->words[NW_BUS_RELEASED]=s.bus_released;r->words[NW_FAULT]=s.fault_latched;
 r->words[NW_RAM_SCRUBBED]=atomic_get(&marker_ram_scrubbed)!=0;
 if(rc&&!r->words[NW_OUTCOME])r->words[NW_OUTCOME]=NWR_REFUSED;
 if(rc&&r->words[NW_OUTCOME]==NWR_VERIFIED)r->words[NW_OUTCOME]=NWR_CLEANUP_ERROR;
 nwr_active_result=NULL;nwr_active_observer=NULL;
done:r->words[NW_RC]=(uint32_t)rc;atomic_clear(&nwr_entry);return rc;
}
