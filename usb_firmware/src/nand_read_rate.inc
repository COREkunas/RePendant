/* Included privately by the fixed public-object adapter. No generic SPI API. */
#include "nand_read_rate.h"
static const uint32_t nand_read_rates[4][2] __attribute__((used,retain)) = {
 {125000,0x02000000},{500000,0x08000000},{1000000,0x10000000},{2000000,0x20000000} };
static const uint8_t nand_read_rate_schedule[NRR_RUNS] __attribute__((used,retain)) =
 {0,0,0,1,1,1,2,2,2,3,3,3,0};
static const uint32_t nand_read_rate_bounds[10] __attribute__((used,retain)) =
 {13,598,20000,2000,46,42,4100,350000,125000,100};
BUILD_ASSERT(NRF_SPIM_FREQ_125K==0x02000000U && NRF_SPIM_FREQ_500K==0x08000000U &&
 NRF_SPIM_FREQ_1M==0x10000000U && NRF_SPIM_FREQ_2M==0x20000000U,
 "Fixed benchmark rates must match Nordic MDK values");
BUILD_ASSERT(NRR_WORDS==72 && NR_EVENT_COUNT==71 && NRR_RUNS*46U==NRR_MAX_STARTS,
 "Fixed metadata and aggregate transfer bound");
static atomic_t nrr_attempted;
struct nrr_context {
 struct nand_read_rate_result *result;
 const struct nand_read_rate_observer *observer;
 int64_t begin,deadline,run_begin;
 uint32_t run_starts;
};
static struct nrr_context nrr_workspace;
static struct nrr_context *nrr_active;
static struct npo_result nrr_npo_result;
static int npo_run_admitted(struct npo_result *,enum npo_mode,const struct npo_observer *);
#define RW (nrr_active->result->words)
static int nrr_fail(enum nrr_outcome outcome,int rc)
{
 if(!RW[NR_OUTCOME]){RW[NR_OUTCOME]=outcome;RW[NR_RC]=(uint32_t)rc;}
 return rc;
}
static int nrr_check(void *user,int64_t ignored)
{
 (void)user;(void)ignored;
 if(k_uptime_get()>=nrr_active->deadline)return nrr_fail(NRR_DEADLINE_ERROR,-ETIMEDOUT);
 int rc=nrr_active->observer->check(nrr_active->observer->user,nrr_active->deadline);
 if(rc){RW[NR_OBSERVER_RC]=(uint32_t)rc;return nrr_fail(NRR_OBSERVER_ERROR,rc);}
 if(k_uptime_get()>=nrr_active->deadline)return nrr_fail(NRR_DEADLINE_ERROR,-ETIMEDOUT);
 return 0;
}
static int nrr_no_event(void *u,const struct npo_result *r,int64_t d)
{(void)u;(void)r;(void)d;return nrr_fail(NRR_RUN_ERROR,-EPERM);}
static const struct npo_observer nrr_npo_observer={NULL,nrr_check,nrr_no_event};
static int nrr_transfer_guard(enum npo_op op,unsigned row,int64_t *deadline)
{
 if(!nrr_active)return 0;
 /* Independent immutable read-only fence, including restoration. */
 if(npo_active_mode!=NPO_RECOVER_ONLY || row>2 ||
    !(op==PO_ID||op==PO_C0||op==PO_A0||op==PO_B0||op==PO_LOAD||op==PO_MAIN))
  return nrr_fail(NRR_RUN_ERROR,-EPERM);
 if(nrr_active->run_starts>=46U||RW[NR_STARTS]>=NRR_MAX_STARTS)
  return nrr_fail(NRR_RUN_ERROR,-EOVERFLOW);
 if(npo_workspace.restoring){
  if(!(op==PO_C0||op==PO_A0||op==PO_B0))return nrr_fail(NRR_RUN_ERROR,-EPERM);
 }else{
  if(nrr_active->run_starts>=42U||RW[NR_STARTS]>=NRR_MAX_STARTS-4U)
   return nrr_fail(NRR_RUN_ERROR,-EOVERFLOW);
  if((op==PO_MAIN||op==PO_LOAD)&&row!=RW[NR_CACHE_STARTS])return nrr_fail(NRR_RUN_ERROR,-EPERM);
  if(*deadline>nrr_active->deadline)*deadline=nrr_active->deadline;
  if(k_uptime_get()>=*deadline)return nrr_fail(NRR_DEADLINE_ERROR,-ETIMEDOUT);
 }
 if(nand_id_spim->FREQUENCY!=NRF_SPIM_FREQ_125K){atomic_set(&fault,1);return nrr_fail(NRR_RATE_ERROR,-EIO);}
 return 0;
}
static int nrr_set_frequency(uint32_t value,bool returning)
{
 if(!nrr_active||!atomic_get(&stopped)||!atomic_get(&cs_high)||!observe_aux()||
    atomic_get(&fault)||!nrf_spim_enable_check(nand_id_spim))return -EPERM;
 nrf_spim_disable(nand_id_spim);
 if(nand_id_spim->ENABLE!=0)goto failed;
 erratum135(false);
 nrf_spim_frequency_set(nand_id_spim,(nrf_spim_frequency_t)value);
 __DMB();
 if(nand_id_spim->FREQUENCY!=value)goto failed;
 erratum135(true);nrf_spim_enable(nand_id_spim);
 if(!nrf_spim_enable_check(nand_id_spim))goto failed;
 __DMB();
 if(returning)++RW[NR_RATE_RESTORES];else ++RW[NR_RATE_SETS];
 return 0;
failed:
 RW[NR_RUN_DEFAULT_VALID]=0;atomic_set(&fault,1);
 return nrr_fail(NRR_RATE_ERROR,-EIO);
}
static int nrr_cache_rate_enter(enum npo_op op)
{
 if(!nrr_active||op!=PO_MAIN)return 0;
 if(RW[NR_CACHE_STARTS]>=3U)return nrr_fail(NRR_RUN_ERROR,-EOVERFLOW);
 uint32_t value=nand_read_rates[RW[NR_RATE_INDEX]][1];
 if(value!=NRF_SPIM_FREQ_125K){int rc=nrr_set_frequency(value,false);if(rc)return rc;}
 RW[NR_RATE_REGISTER]=nand_id_spim->FREQUENCY;
 RW[NR_RUN_DEFAULT_VALID]=value==NRF_SPIM_FREQ_125K;
 return 0;
}
static int nrr_cache_rate_leave(void)
{
 if(!nrr_active)return 0;
 if(!atomic_get(&stopped)||!atomic_get(&cs_high))return -EBUSY;
 if(nand_id_spim->FREQUENCY!=NRF_SPIM_FREQ_125K){int rc=nrr_set_frequency(NRF_SPIM_FREQ_125K,true);if(rc)return rc;}
 RW[NR_FINAL_REGISTER]=nand_id_spim->FREQUENCY;
 RW[NR_RUN_DEFAULT_VALID]=RW[NR_FINAL_REGISTER]==NRF_SPIM_FREQ_125K;
 return RW[NR_RUN_DEFAULT_VALID]?0:nrr_fail(NRR_RATE_ERROR,-EIO);
}
static void nrr_cleanup_check(void)
{
 if(!nrr_active)return;
 if(!atomic_get(&stopped)){RW[NR_RUN_DEFAULT_VALID]=0;return;}
 RW[NR_FINAL_REGISTER]=nand_id_spim->FREQUENCY;
 RW[NR_RUN_DEFAULT_VALID]=RW[NR_FINAL_REGISTER]==NRF_SPIM_FREQ_125K;
 if(nrr_active->run_starts&&!RW[NR_RUN_DEFAULT_VALID]){
  atomic_set(&fault,1);(void)nrr_fail(NRR_RATE_ERROR,-EIO);
 }
}
static void nrr_snapshot(void)
{
 struct nand_id_state s;nand_id_get_state(&s);
 RW[NR_FAULT]=s.fault_latched;RW[NR_STOPPED]=s.clock_stopped;
 RW[NR_BUS_RELEASED]=s.bus_released;RW[NR_RAM_SCRUBBED]=atomic_get(&marker_ram_scrubbed)!=0;
 RW[NR_DEFAULT_VALID]=RW[NR_RUN_DEFAULT_VALID];
 RW[NR_ELAPSED_MS]=(uint32_t)(k_uptime_get()-nrr_active->begin);
}
int nand_read_rate_run(struct nand_read_rate_result *result,const struct nand_read_rate_observer *observer)
{
 if(!result||!observer||!observer->check||!observer->event)return -EINVAL;
 if(!atomic_cas(&npo_entry,0,1))return -EBUSY;
 memset(result,0,sizeof(*result));memset(&nrr_workspace,0,sizeof(nrr_workspace));
 nrr_workspace.result=result;nrr_workspace.observer=observer;
 nrr_workspace.begin=k_uptime_get();nrr_workspace.deadline=nrr_workspace.begin+NRR_DATA_MS;
 nrr_active=&nrr_workspace;
 RW[NR_RUN_INDEX]=UINT32_MAX;RW[NR_RATE_INDEX]=UINT32_MAX;RW[NR_QUARANTINED]=1;
 if(!atomic_cas(&nrr_attempted,0,1)){nrr_fail(NRR_REFUSED,-EALREADY);goto done;}
 RW[NR_ATTEMPTED]=1;
 for(unsigned run=0;run<NRR_RUNS;run++){
  if(nrr_check(NULL,nrr_active->deadline))break;
  memset(RW,0,(NPO_WORDS+16U)*sizeof(uint32_t));
  RW[NR_RUN_INDEX]=run;RW[NR_RATE_INDEX]=nand_read_rate_schedule[run];
  RW[NR_RATE_HZ]=nand_read_rates[RW[NR_RATE_INDEX]][0];
  RW[NR_CYCLE_HZ]=sys_clock_hw_cycles_per_sec();
  nrr_active->run_starts=0;nrr_active->run_begin=k_uptime_get();++RW[NR_RUNS_STARTED];
  int rc=npo_run_admitted(&nrr_npo_result,NPO_RECOVER_ONLY,&nrr_npo_observer);
  memcpy(RW,nrr_npo_result.words,sizeof(nrr_npo_result.words));
  RW[NR_RUN_WALL_MS]=(uint32_t)(k_uptime_get()-nrr_active->run_begin);nrr_snapshot();
  if(rc){nrr_fail(NRR_RUN_ERROR,rc);break;}
  if(!RW[NR_RUN_DEFAULT_VALID]||!RW[NR_STOPPED]||!RW[NR_BUS_RELEASED]||!RW[NR_RAM_SCRUBBED]||RW[NR_FAULT]||
     RW[NR_CACHE_STARTS]!=3||RW[NR_CACHE_COMPLETED]!=3||RW[NR_TIMING_VALID]!=7){nrr_fail(NRR_CLEANUP_ERROR,-EIO);break;}
  if(nrr_check(NULL,nrr_active->deadline))break;
  ++RW[NR_RUNS_VERIFIED];++RW[NR_EVENT_COUNT];
  int64_t limit=k_uptime_get()+NRR_EVENT_MS;if(limit>nrr_active->deadline)limit=nrr_active->deadline;
  rc=observer->event(observer->user,result,limit);
  if(rc){RW[NR_OBSERVER_RC]=(uint32_t)rc;nrr_fail(NRR_OBSERVER_ERROR,rc);break;}
  if(k_uptime_get()>=limit){nrr_fail(NRR_DEADLINE_ERROR,-ETIMEDOUT);break;}
  ++RW[NR_RUNS_ACKED];
 }
 if(!RW[NR_OUTCOME]){
  if(nrr_check(NULL,nrr_active->deadline)==0)RW[NR_OUTCOME]=NRR_VERIFIED;
 }
done:
 nrr_snapshot();int rc=(int32_t)RW[NR_RC];
 nrr_active=NULL;nrr_workspace.result=NULL;nrr_workspace.observer=NULL;
 atomic_clear(&npo_entry);return rc;
}
#undef RW
