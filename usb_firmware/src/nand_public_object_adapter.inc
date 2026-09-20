/* Included privately by nand_id.c, after common bounded bus primitives.
 * No public raw-SPI entrypoint. CS15 only; reads only block1024 and programs
 * exactly its already-sacrificed rows1/2. No erase opcode exists here. */
static atomic_t npo_entry;
static struct npo_core npo_workspace;
static __aligned(4) uint8_t nand_public_program_tx[4099];
BUILD_ASSERT(sizeof(nand_public_program_tx)==4096U+3U &&
             sizeof(nand_public_program_tx)<=
             (SPIM_TXD_MAXCNT_MAXCNT_Msk>>SPIM_TXD_MAXCNT_MAXCNT_Pos),
             "Fixed main-only program DMA must fit actual Nordic MAXCNT");
static struct npo_result *npo_active_result;
static const struct npo_observer *npo_active_observer;
static enum npo_mode npo_active_mode;
static bool npo_previous_clean;
static bool npo_reentry_pins(void)
{
 if(!npo_previous_clean||!atomic_get(&stopped)||!atomic_get(&bus_released)||!observe_aux()||!atomic_get(&cs_high))return false;
 for(size_t i=0;i<3;i++)if(NRF_P1->PIN_CNF[nand_id_bus_pins[i]-32U]!=2U)return false;
 for(size_t i=0;i<4;i++)if(NRF_P1->PIN_CNF[nand_id_cs_pins[i]-32U]!=3U||nrf_gpio_pin_out_read(nand_id_cs_pins[i])!=1U)return false;
 return NRF_P1->PIN_CNF[nand_id_aux_pin-32U]==3U;
}
static const uint32_t nand_public_bounds[13] __attribute__((used,retain)) =
 {1024,65536,65537,65538,64,4096,4352,4099,1024,60000,100,350000,5000};
static const uint8_t nand_public_ops[PO_OP_COUNT][4] __attribute__((used,retain)) = {
 {0x9f,0,0,0},{0x0f,0xc0,0,0},{0x0f,0xa0,0,0},{0x0f,0xb0,0,0},
 {0x1f,0xb0,0,0},{0x1f,0xb0,0x10,0},{0x1f,0xa0,0x48,0},{0x1f,0xa0,0x7c,0},
 {0x13,1,0,0},{0x03,0,0,0},{0x03,0,0,0},{0x06,0,0,0},{0x04,0,0,0},
 {0x02,0,0,0},{0x02,0,0,0},{0x10,1,0,1},{0x10,1,0,2} };
static int64_t npo_hw_now(void *user){(void)user;return k_uptime_get();}
static void npo_hw_wait(void *user,unsigned us){(void)user;k_busy_wait(us);}
static bool npo_hw_safe(void *user)
{(void)user;return atomic_get(&stopped)&&!atomic_get(&fault)&&atomic_get(&cs_high)&&observe_aux();}
static void npo_hw_wipe(void *user)
{
 (void)user;if(!atomic_get(&stopped))return;
 block1024_wipe();volatile uint8_t *p=nand_public_program_tx;
 for(size_t i=0;i<sizeof(nand_public_program_tx);i++)p[i]=0;
}
static int npo_hw_hash(void *user,const uint8_t *input,size_t n,uint8_t *out,size_t cap,size_t *written)
{
 (void)user;*written=0;
 psa_hash_operation_t h=PSA_HASH_OPERATION_INIT;
 psa_status_t rc=psa_hash_setup(&h,PSA_ALG_SHA_256);
 if(rc==PSA_SUCCESS)rc=psa_hash_update(&h,input,n);
 if(rc==PSA_SUCCESS)rc=psa_hash_finish(&h,out,cap,written);
 psa_status_t aborted=psa_hash_abort(&h);
 if(aborted!=PSA_SUCCESS)atomic_set(&fault,1);
 return rc==PSA_SUCCESS&&aborted==PSA_SUCCESS&&*written==32?0:-EIO;
}
#include "nand_read_rate.inc"
static bool npo_hw_allowed(enum npo_op op,unsigned row)
{
 const uint32_t *w=npo_active_result->words;
 if((unsigned)op>=PO_OP_COUNT||row>=64||w[NP_TRANSFERS]>=NPO_MAX_STARTS)return false;
 if(npo_workspace.restoring){
  if(!npo_hw_safe(NULL)||w[NP_READY_UNKNOWN])return false;
  return op==PO_C0||op==PO_A0||op==PO_B0||
   (op==PO_LOCK&&w[NP_A0_DIRTY])||(op==PO_ON&&w[NP_B0_DIRTY])||
   (op==PO_WRDI&&w[NP_WREN_STARTS]&&!w[NP_WRDI_STARTS]&&w[NP_C0_VALID]&&(w[NP_C0]&2));
 }
 if(op==PO_ID||op==PO_C0||op==PO_A0||op==PO_B0)return true;
 if(op==PO_LOAD)return !w[NP_READY_UNKNOWN]&&(npo_active_mode==NPO_WRITE_ONCE||row<=2);
 if(op==PO_RAW)return npo_active_mode==NPO_WRITE_ONCE&&w[NP_PHASE]==NPO_TAIL&&w[NP_B0_VALID]&&!w[NP_B0]&&!w[NP_READY_UNKNOWN];
 if(op==PO_MAIN)return row<=2&&w[NP_B0_VALID]&&w[NP_B0]==0x10&&!w[NP_READY_UNKNOWN];
 if(npo_active_mode!=NPO_WRITE_ONCE||!w[NP_WRITE_INTENT]||!w[NP_PATTERN_VALID])return false;
 if(op==PO_OFF)return !w[NP_B0_DIRTY]&&w[NP_B0_VALID]&&w[NP_B0]==0x10;
 if(op==PO_ON)return w[NP_BLANK_ROWS]==63&&w[NP_B0_VALID]&&!w[NP_B0];
 if(op==PO_UNLOCK)return w[NP_BLANK_ROWS]==63&&w[NP_A0_VALID]&&w[NP_A0]==0x7c&&w[NP_B0_VALID]&&w[NP_B0]==0x10;
 if(w[NP_BLANK_ROWS]!=63||!w[NP_A0_VALID]||w[NP_A0]!=0x48||!w[NP_B0_VALID]||w[NP_B0]!=0x10||!w[NP_C0_VALID])return false;
 if(op==PO_WREN)return (w[NP_C0]&0x8f)==0&&w[NP_WREN_STARTS]<2&&row==w[NP_WREN_STARTS]+1;
 if((w[NP_C0]&0x8f)!=2)return false;
 if(op==PO_DATA_LOAD)return row==1&&w[NP_WREN_STARTS]==1&&!w[NP_PROGRAM_LOADS];
 if(op==PO_COMMIT_LOAD)return row==2&&w[NP_WREN_STARTS]==2&&w[NP_PROGRAM_LOADS]==1&&w[NP_PROGRAM_COMPLETED]==1;
 if(op==PO_DATA_EXEC)return row==1&&w[NP_PROGRAM_LOADS]==1&&!w[NP_PROGRAM_EXECUTES];
 if(op==PO_COMMIT_EXEC)return row==2&&w[NP_PROGRAM_LOADS]==2&&w[NP_PROGRAM_EXECUTES]==1&&w[NP_PROGRAM_COMPLETED]==1;
 return false;
}
static int npo_hw_transfer(void *user,enum npo_op op,unsigned row,int64_t deadline,struct npo_reply *r)
{
 (void)user;r->stopped=atomic_get(&stopped)!=0;
 if(!npo_active_result||!npo_hw_allowed(op,row)||!npo_hw_safe(NULL))return -EPERM;
 int rate_rc=nrr_transfer_guard(op,row,&deadline);if(rate_rc)return rate_rc;
 bool cache=op==PO_RAW||op==PO_MAIN,program=op==PO_DATA_LOAD||op==PO_COMMIT_LOAD;
 bool long_dma=cache||program;
 size_t n=op==PO_RAW?4356:op==PO_MAIN?4100:program?4099:op==PO_WREN||op==PO_WRDI?1:op==PO_ID||op==PO_LOAD||op==PO_DATA_EXEC||op==PO_COMMIT_EXEC?4:3;
 uint8_t *tx=program?nand_public_program_tx:cache?nand_marker_tx:nand_id_tx;
 uint8_t *rx=long_dma?nand_marker_rx_first:nand_id_rx;
 if(k_uptime_get()>=deadline)return -ETIMEDOUT;
 if(program){memcpy(tx,nand_public_ops[op],3);memcpy(tx+3,op==PO_DATA_LOAD?npo_workspace.data:npo_workspace.commit,4096);}
 else if(!cache){memcpy(tx,nand_public_ops[op],4);if(op==PO_LOAD)tx[3]=(uint8_t)row;}
 memset(rx,0,n);
 nrf_spim_event_clear(nand_id_spim,NRF_SPIM_EVENT_END);
 nrf_spim_event_clear(nand_id_spim,NRF_SPIM_EVENT_STOPPED);
 nrf_spim_tx_buffer_set(nand_id_spim,tx,n);nrf_spim_rx_buffer_set(nand_id_spim,rx,n);
 if(!all_cs_high())return -EIO;
 /* Copying a full public page and preparing DMA must not hide a lifecycle
  * failure since the core's check. Restoration deliberately ignores USB. */
 if(!npo_workspace.restoring){
  int rc=npo_active_observer->check(npo_active_observer->user,npo_workspace.deadline);
  if(rc){
   uint32_t *w=npo_active_result->words;w[NP_OBSERVER_RC]=(uint32_t)rc;
   if(!w[NP_PRIMARY_OUTCOME]){w[NP_PRIMARY_OUTCOME]=NPO_OBSERVER_ERROR;w[NP_PRIMARY_RC]=(uint32_t)rc;}
   return rc;
  }
 }
 rate_rc=nrr_cache_rate_enter(op);if(rate_rc)return rate_rc;
 /* Recheck after controller reconfiguration; no expired/invalidated START. */
 if(nrr_active&&!npo_workspace.restoring){
  rate_rc=nrr_check(NULL,nrr_active->deadline);
  if(rate_rc){(void)nrr_cache_rate_leave();return rate_rc;}
 }
 nrf_gpio_pin_clear(nand_id_cs_pins[1]);atomic_clear(&cs_high);k_busy_wait(2);
 if(k_uptime_get()>=deadline){(void)all_cs_high();if(nrr_active)(void)nrr_cache_rate_leave();return -ETIMEDOUT;}
 atomic_clear(&stopped);if(long_dma)atomic_clear(&marker_ram_scrubbed);
 uint32_t cycle_begin=0;
 if(nrr_active){++nrr_active->run_starts;++nrr_active->result->words[NR_STARTS];
  nrr_active->result->words[NR_RUN_DEFAULT_VALID]=0;
  if(op==PO_MAIN){++nrr_active->result->words[NR_CACHE_STARTS];cycle_begin=k_cycle_get_32();}}
 __DMB();nrf_spim_task_trigger(nand_id_spim,NRF_SPIM_TASK_START);r->started=true;r->stopped=false;
 bool ended=long_dma?wait_cache_end(deadline):wait_event(NRF_SPIM_EVENT_END,deadline);
 uint32_t cycle_elapsed=nrr_active&&op==PO_MAIN?(uint32_t)(k_cycle_get_32()-cycle_begin):0;
 bool high=all_cs_high();
 nrf_spim_event_clear(nand_id_spim,NRF_SPIM_EVENT_STOPPED);nrf_spim_task_trigger(nand_id_spim,NRF_SPIM_TASK_STOP);
 if(!wait_event(NRF_SPIM_EVENT_STOPPED,k_uptime_get()+3)){atomic_set(&fault,1);return -EBUSY;}
 __DMB();atomic_set(&stopped,1);r->stopped=true;
 r->tx_amount=nrf_spim_tx_amount_get(nand_id_spim);r->rx_amount=nrf_spim_rx_amount_get(nand_id_spim);
 if(nrr_active){
  if(op==PO_MAIN&&ended&&high&&r->tx_amount==n&&r->rx_amount==n){
   uint32_t *w=nrr_active->result->words;unsigned i=w[NR_CACHE_STARTS]-1U;
   w[NR_CACHE0_CYCLES+i]=cycle_elapsed;w[NR_TIMING_VALID]|=1U<<i;++w[NR_CACHE_COMPLETED];
  }
  rate_rc=nrr_cache_rate_leave();if(rate_rc)return rate_rc;
 }
 if(!ended||!high||r->tx_amount!=n||r->rx_amount!=n)return -EIO;
 r->bytes=rx;return 0;
}
static int npo_adapter_run(void)
{
 const struct npo_io io={NULL,npo_hw_now,npo_hw_wait,npo_hw_transfer,npo_hw_safe,npo_hw_wipe,
  nand_qualify_program_tx+3,{NULL,npo_hw_hash}};
 int rc=npo_core_run(&npo_workspace,npo_active_result,npo_active_mode,&io,npo_active_observer);
 npo_workspace.result=NULL;npo_workspace.io=NULL;npo_workspace.observer=NULL;
 if(npo_active_result->words[NP_FAULT]||npo_active_result->words[NP_READY_UNKNOWN]||npo_active_result->words[NP_A0_DIRTY]||npo_active_result->words[NP_B0_DIRTY])atomic_set(&fault,1);
 return rc;
}
